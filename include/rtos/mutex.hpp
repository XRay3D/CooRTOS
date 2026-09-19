// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>

#include "kernel.hpp"

namespace rtos {

// Мьютекс для задач. Из ISR не использовать. Наследования приоритетов нет:
// при кооперативном планировщике удержание мьютекса не прерывается, инверсия
// возможна только между точками co_await — учитывайте при выборе приоритетов.
// unlock() передаёт владение первому ждущему напрямую (FIFO).
class Mutex {
public:
    constexpr Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    struct LockAwaiter {
        Mutex& m;
        WaitNode w;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (!m.owner_) {
                m.owner_ = kernel::current;
                return false;
            }
            RTOS_CONTRACT_ASSERT(m.owner_ != kernel::current); // не рекурсивный
            kernel::block_current_locked(w, &m.waiters_, h);
            return true;
        }
        void await_resume() noexcept {}
    };

    [[nodiscard]] LockAwaiter lock() { return {*this, {}}; }

    bool try_lock() RTOS_PRE(kernel::current != nullptr) /* только из задач */ {
        port::CriticalSection cs;
        RTOS_ASSERT(kernel::current != nullptr); // остаётся и при ignore
        if (owner_) {
            return false;
        }
        owner_ = kernel::current;
        return true;
    }

    void unlock()
        RTOS_PRE(kernel::current != nullptr && owner_ == kernel::current) {
        port::CriticalSection cs;
        RTOS_ASSERT(kernel::current != nullptr &&
                    owner_ == kernel::current); // остаётся и при ignore
        if (ListNode* n = waiters_.pop_front()) {
            WaitNode* w = static_cast<WaitNode*>(n->owner);
            owner_ = w->task; // владение передано, никто не «вклинится»
            kernel::wake_locked(w);
        } else {
            owner_ = nullptr;
        }
    }

    bool locked() const {
        port::CriticalSection cs;
        return owner_ != nullptr;
    }

private:
    TaskPromise* owner_ = nullptr;
    List waiters_;
};

} // namespace rtos
