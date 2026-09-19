// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>
#include <cstddef>
#include <new>
#include <optional>
#include <utility>

#include "kernel.hpp"

namespace rtos {

// Очередь фиксированной ёмкости (аналог xQueue).
//  - co_await q.receive()            -> T      (ждать, пока пусто)
//  - co_await q.receive_for(t)       -> std::optional<T>
//  - co_await q.send(v)              -> void   (ждать, пока полно)
//  - co_await q.send_for(v, t)       -> bool
//  - try_send / try_receive          — неблокирующие, из задач
//  - try_send_from_isr / try_receive_from_isr — из обработчиков прерываний
//
// Значение передаётся ждущему напрямую (direct handoff): отправитель
// конструирует T в слоте awaiter'а получателя, минуя буфер.
template <class T, std::size_t N>
class Queue {
    static_assert(N > 0, "Queue capacity must be positive");

public:
    Queue() = default;
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    ~Queue() {
        while (count_ > 0) {
            pop_slot_locked();
        }
    }

    // --- неблокирующие / ISR ---

    bool try_send(T v) {
        port::CriticalSection cs;
        return deliver_or_push_locked(v);
    }
    bool try_send_from_isr(T v) { return try_send(std::move(v)); }

    bool try_receive(T& out) {
        port::CriticalSection cs;
        if (count_ == 0) {
            return false;
        }
        out = pop_slot_locked();
        wake_one_sender_locked();
        return true;
    }
    bool try_receive_from_isr(T& out) { return try_receive(out); }

    std::size_t size() const {
        port::CriticalSection cs;
        return count_;
    }
    static constexpr std::size_t capacity() { return N; }

    // --- awaitable ---

    struct ReceiveAwaiter {
        Queue& q;
        WaitNode w;
        alignas(T) std::byte storage[sizeof(T)];
        bool got = false;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (q.count_ > 0) {
                ::new (static_cast<void*>(storage)) T(q.pop_slot_locked());
                got = true;
                q.wake_one_sender_locked();
                return false;
            }
            w.payload = storage;
            kernel::block_current_locked(w, &q.receivers_, h);
            return true;
        }
        T await_resume() {
            T* p = std::launder(reinterpret_cast<T*>(storage));
            T v = std::move(*p);
            p->~T();
            return v;
        }
    };

    struct ReceiveForAwaiter {
        Queue& q;
        Tick timeout;
        WaitNode w;
        alignas(T) std::byte storage[sizeof(T)];
        bool got = false;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (q.count_ > 0) {
                ::new (static_cast<void*>(storage)) T(q.pop_slot_locked());
                got = true;
                q.wake_one_sender_locked();
                return false;
            }
            w.payload = storage;
            kernel::block_current_locked(w, &q.receivers_, h,
                                         /*has_deadline=*/true,
                                         kernel::now() + timeout);
            return true;
        }
        std::optional<T> await_resume() {
            if (!got && w.timed_out) {
                return std::nullopt;
            }
            T* p = std::launder(reinterpret_cast<T*>(storage));
            std::optional<T> v(std::move(*p));
            p->~T();
            return v;
        }
    };

    struct SendAwaiter {
        Queue& q;
        T value;
        WaitNode w;
        bool done = false;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (q.deliver_or_push_locked(value)) {
                done = true;
                return false;
            }
            w.payload = &value;
            kernel::block_current_locked(w, &q.senders_, h);
            return true;
        }
        void await_resume() noexcept {}
    };

    struct SendForAwaiter {
        Queue& q;
        T value;
        Tick timeout;
        WaitNode w;
        bool done = false;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (q.deliver_or_push_locked(value)) {
                done = true;
                return false;
            }
            w.payload = &value;
            kernel::block_current_locked(w, &q.senders_, h,
                                         /*has_deadline=*/true,
                                         kernel::now() + timeout);
            return true;
        }
        bool await_resume() noexcept { return done || !w.timed_out; }
    };

    [[nodiscard]] ReceiveAwaiter receive() { return {*this, {}, {}, false}; }
    [[nodiscard]] ReceiveForAwaiter receive_for(Tick timeout) {
        return {*this, timeout, {}, {}, false};
    }
    [[nodiscard]] SendAwaiter send(T v) {
        return {*this, std::move(v), {}, false};
    }
    [[nodiscard]] SendForAwaiter send_for(T v, Tick timeout) {
        return {*this, std::move(v), timeout, {}, false};
    }

    template <class Rep, class Period>
    [[nodiscard]] ReceiveForAwaiter
    receive_for(std::chrono::duration<Rep, Period> t) {
        return receive_for(to_ticks(t));
    }
    template <class Rep, class Period>
    [[nodiscard]] SendForAwaiter send_for(T v,
                                          std::chrono::duration<Rep, Period> t) {
        return send_for(std::move(v), to_ticks(t));
    }

private:
    T* slot(std::size_t i) {
        return std::launder(reinterpret_cast<T*>(buf_ + i * sizeof(T)));
    }

    void push_slot_locked(T&& v) RTOS_PRE(port::in_critical()) {
        RTOS_CONTRACT_ASSERT(count_ < N);
        ::new (static_cast<void*>(buf_ + ((head_ + count_) % N) * sizeof(T)))
            T(std::move(v));
        ++count_;
    }

    T pop_slot_locked() RTOS_PRE(port::in_critical()) {
        RTOS_CONTRACT_ASSERT(count_ > 0);
        T* p = slot(head_);
        T v = std::move(*p);
        p->~T();
        head_ = (head_ + 1) % N;
        --count_;
        return v;
    }

    // Отдать значение ждущему получателю либо положить в буфер.
    // false — очередь полна и получателей нет.
    bool deliver_or_push_locked(T& v) RTOS_PRE(port::in_critical()) {
        if (ListNode* n = receivers_.pop_front()) {
            WaitNode* rw = static_cast<WaitNode*>(n->owner);
            ::new (rw->payload) T(std::move(v));
            kernel::wake_locked(rw);
            return true;
        }
        if (count_ < N) {
            push_slot_locked(std::move(v));
            return true;
        }
        return false;
    }

    // После освобождения места — переложить значение заблокированного
    // отправителя в буфер и разбудить его.
    void wake_one_sender_locked() RTOS_PRE(port::in_critical()) {
        if (ListNode* n = senders_.pop_front()) {
            WaitNode* sw = static_cast<WaitNode*>(n->owner);
            push_slot_locked(std::move(*static_cast<T*>(sw->payload)));
            kernel::wake_locked(sw);
        }
    }

    alignas(T) std::byte buf_[N * sizeof(T)];
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    List receivers_;
    List senders_;
};

} // namespace rtos
