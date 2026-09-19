// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>

#include "kernel.hpp"

namespace rtos {

// co_await sleep_for(ticks): усыпить текущую задачу на ticks тиков
// (при SysTick 1 кГц — миллисекунд). Вырожденный случай ожидания:
// узел только в delay-списке.
struct SleepAwaiter {
    Tick ticks;
    WaitNode w;

    bool await_ready() const noexcept { return ticks == 0; }
    void await_suspend(std::coroutine_handle<> h) {
        port::CriticalSection cs;
        kernel::block_current_locked(w, nullptr, h, /*has_deadline=*/true,
                                     kernel::now() + ticks);
    }
    void await_resume() noexcept {}
};

inline SleepAwaiter sleep_for(Tick ticks) { return {ticks, {}}; }

template <class Rep, class Period>
SleepAwaiter sleep_for(std::chrono::duration<Rep, Period> d) {
    return {to_ticks(d), {}};
}

// Позволяет писать co_await 500ms: await_transform промиса превращает
// длительность в SleepRequest, а этот оператор — в обычный sleep-awaiter.
inline SleepAwaiter operator co_await(SleepRequest r) { return {r.ticks, {}}; }

} // namespace rtos
