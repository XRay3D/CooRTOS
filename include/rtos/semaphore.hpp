// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>
#include <cstdint>

#include "kernel.hpp"

namespace rtos {

// Счётный семафор. release() при наличии ждущего будит его напрямую,
// не увеличивая счётчик (direct handoff, FIFO-честность): значение не
// может быть украдено промежуточным try_acquire.
class CountingSemaphore {
public:
    constexpr CountingSemaphore(std::uint32_t initial, std::uint32_t max_count)
        : count_(initial), max_(max_count) {}
    CountingSemaphore(const CountingSemaphore&) = delete;
    CountingSemaphore& operator=(const CountingSemaphore&) = delete;

    struct AcquireAwaiter {
        CountingSemaphore& s;
        WaitNode w;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (s.count_ > 0) {
                --s.count_;
                return false;
            }
            kernel::block_current_locked(w, &s.waiters_, h);
            return true;
        }
        void await_resume() noexcept {}
    };

    struct AcquireForAwaiter {
        CountingSemaphore& s;
        Tick timeout;
        WaitNode w;
        bool got = false;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (s.count_ > 0) {
                --s.count_;
                got = true;
                return false;
            }
            kernel::block_current_locked(w, &s.waiters_, h,
                                         /*has_deadline=*/true,
                                         kernel::now() + timeout);
            return true;
        }
        bool await_resume() noexcept { return got || !w.timed_out; }
    };

    [[nodiscard]] AcquireAwaiter acquire() { return {*this, {}}; }
    [[nodiscard]] AcquireForAwaiter try_acquire_for(Tick timeout) {
        return {*this, timeout, {}, false};
    }
    template <class Rep, class Period>
    [[nodiscard]] AcquireForAwaiter
    try_acquire_for(std::chrono::duration<Rep, Period> t) {
        return try_acquire_for(to_ticks(t));
    }

    bool try_acquire() {
        port::CriticalSection cs;
        if (count_ > 0) {
            --count_;
            return true;
        }
        return false;
    }

    // false — счётчик уже на максимуме и ждущих нет (release потерян).
    bool release() {
        port::CriticalSection cs;
        if (ListNode* n = waiters_.pop_front()) {
            kernel::wake_locked(static_cast<WaitNode*>(n->owner));
            return true;
        }
        if (count_ < max_) {
            ++count_;
            return true;
        }
        return false;
    }

    bool release_from_isr() { return release(); }

    std::uint32_t count() const {
        port::CriticalSection cs;
        return count_;
    }

private:
    std::uint32_t count_;
    std::uint32_t max_;
    List waiters_;
};

class BinarySemaphore : public CountingSemaphore {
public:
    explicit constexpr BinarySemaphore(bool available = false)
        : CountingSemaphore(available ? 1 : 0, 1) {}
};

} // namespace rtos
