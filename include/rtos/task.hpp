// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <new>
#include <ratio>
#include <utility>

#include "config.hpp"
#include "intrusive_list.hpp"
#include "pool.hpp"

namespace rtos {

// std::chrono-длительность -> тики, с округлением вверх (спать/ждать
// не меньше запрошенного). Отрицательное и ноль -> 0 тиков.
template <class Rep, class Period>
constexpr Tick to_ticks(std::chrono::duration<Rep, Period> d) {
    if (d <= std::chrono::duration<Rep, Period>::zero()) {
        return 0;
    }
    using TickDuration =
        std::chrono::duration<Tick, std::ratio<1, RTOS_TICK_HZ>>;
    return std::chrono::ceil<TickDuration>(d).count();
}

// Результат await_transform для chrono-длительностей; его operator co_await
// (time.hpp) превращает co_await 500ms в co_await sleep_for(500).
struct SleepRequest {
    Tick ticks;
};

class Task;

// Промис верхнеуровневой задачи. Задача detached: после kernel::start()
// владелец — планировщик, фрейм уничтожается в final_suspend.
struct TaskPromise {
    enum class State : std::uint8_t { Created, Ready, Running, Blocked };

    ListNode ready_link{this};
    // Innermost-handle для возобновления: сама задача либо вложенная Async,
    // заблокировавшаяся внутри неё.
    std::coroutine_handle<> resume_point;
    std::uint8_t priority = RTOS_NUM_PRIORITIES - 1;
    State state = State::Created;

    Task get_return_object();
    static Task get_return_object_on_allocation_failure();

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<TaskPromise> h) noexcept {
            h.destroy(); // задача завершилась — вернуть блок пула
        }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() noexcept {}
    void unhandled_exception() noexcept { RTOS_TRAP(); }

    // co_await <chrono-длительность> == co_await sleep_for(...); остальные
    // awaitable проходят насквозь (стандартный passthrough-паттерн).
    template <class A>
    A&& await_transform(A&& a) noexcept {
        return static_cast<A&&>(a);
    }
    template <class Rep, class Period>
    SleepRequest await_transform(std::chrono::duration<Rep, Period> d) {
        return SleepRequest{to_ticks(d)};
    }

    void* operator new(std::size_t n) noexcept { return config::allocate(n); }
    void operator delete(void* p, std::size_t n) noexcept {
        config::deallocate(p, n);
    }
    void operator delete(void* p) noexcept { config::deallocate(p, 0); }
};

// Move-only хэндл незапущенной задачи; kernel::start() забирает владение.
// Невалиден (operator bool == false) при неудачной аллокации фрейма.
class Task {
public:
    using promise_type = TaskPromise;

    Task(Task&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (h_) {
            h_.destroy(); // так и не была запущена
        }
    }

    explicit operator bool() const { return h_ != nullptr; }

    std::coroutine_handle<TaskPromise> release() {
        return std::exchange(h_, nullptr);
    }

private:
    friend struct TaskPromise;
    explicit Task(std::coroutine_handle<TaskPromise> h) : h_(h) {}

    std::coroutine_handle<TaskPromise> h_;
};

inline Task TaskPromise::get_return_object() {
    return Task{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

inline Task TaskPromise::get_return_object_on_allocation_failure() {
    return Task{nullptr};
}

namespace detail {

template <class Promise>
struct AsyncPromiseBase {
    std::coroutine_handle<> continuation;

    std::suspend_always initial_suspend() noexcept { return {}; } // lazy

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<>
        await_suspend(std::coroutine_handle<Promise> h) noexcept {
            // symmetric transfer обратно в ожидающую корутину
            return h.promise().continuation;
        }
        void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { RTOS_TRAP(); }

    template <class A>
    A&& await_transform(A&& a) noexcept {
        return static_cast<A&&>(a);
    }
    template <class Rep, class Period>
    SleepRequest await_transform(std::chrono::duration<Rep, Period> d) {
        return SleepRequest{to_ticks(d)};
    }

    void* operator new(std::size_t n) noexcept { return config::allocate(n); }
    void operator delete(void* p, std::size_t n) noexcept {
        config::deallocate(p, n);
    }
    void operator delete(void* p) noexcept { config::deallocate(p, 0); }
};

} // namespace detail

// Вложенная корутина с результатом: co_await из Task или другой Async.
// Ядра не касается — блокирующие awaiter'ы внутри неё сами сохраняют
// innermost-handle в TaskPromise::resume_point текущей задачи.
template <class T>
class [[nodiscard]] Async {
public:
    struct promise_type : detail::AsyncPromiseBase<promise_type> {
        alignas(T) std::byte result[sizeof(T)];
        bool has_result = false;

        Async get_return_object() {
            return Async{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        static Async get_return_object_on_allocation_failure() {
            return Async{nullptr};
        }

        void return_value(T v) {
            ::new (static_cast<void*>(result)) T(std::move(v));
            has_result = true;
        }

        ~promise_type() {
            if (has_result) {
                std::launder(reinterpret_cast<T*>(result))->~T();
            }
        }
    };

    Async(Async&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;

    ~Async() {
        if (h_) {
            h_.destroy();
        }
    }

    auto operator co_await() && {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;
            bool await_ready() {
                RTOS_ASSERT(h != nullptr); // аллокация фрейма не удалась
                return false;
            }
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<> cont) {
                h.promise().continuation = cont;
                return h; // запустить вложенную корутину
            }
            T await_resume() {
                return std::move(
                    *std::launder(reinterpret_cast<T*>(h.promise().result)));
            }
        };
        return Awaiter{h_};
    }

private:
    friend promise_type;
    explicit Async(std::coroutine_handle<promise_type> h) : h_(h) {}

    std::coroutine_handle<promise_type> h_;
};

template <>
class [[nodiscard]] Async<void> {
public:
    struct promise_type : detail::AsyncPromiseBase<promise_type> {
        Async get_return_object() {
            return Async{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        static Async get_return_object_on_allocation_failure() {
            return Async{nullptr};
        }
        void return_void() noexcept {}
    };

    Async(Async&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;

    ~Async() {
        if (h_) {
            h_.destroy();
        }
    }

    auto operator co_await() && {
        struct Awaiter {
            std::coroutine_handle<promise_type> h;
            bool await_ready() {
                RTOS_ASSERT(h != nullptr);
                return false;
            }
            std::coroutine_handle<>
            await_suspend(std::coroutine_handle<> cont) {
                h.promise().continuation = cont;
                return h;
            }
            void await_resume() {}
        };
        return Awaiter{h_};
    }

private:
    friend promise_type;
    explicit Async(std::coroutine_handle<promise_type> h) : h_(h) {}

    std::coroutine_handle<promise_type> h_;
};

} // namespace rtos
