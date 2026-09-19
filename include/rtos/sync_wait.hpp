// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "kernel.hpp"
#include "task.hpp"

namespace rtos {

// Мост из не-корутинного контекста в Async<T>: выполнить вложенную корутину
// до конца, крутя планировщик через pump (обычно run_one() + тик + idle,
// либо pump-хук приложения), и вернуть её результат.
//
//   T v = rtos::sync_wait(do_work(), [] { while (kernel::run_one()) {} idle(); });
//
// Async оборачивается в detached-задачу с приоритетом prio; pump зовётся
// в цикле, пока задача не завершилась. Годится и из главного цикла (когда
// kernel::run() не используется), и из синхронного кода, вызванного самой
// задачей — run_one() реентерабелен, вызывающая задача Running и не
// повторяется. Из ISR звать нельзя.

namespace detail {

template <class T>
struct SyncSlot {
    std::optional<T> value;
    bool done = false;
};

template <>
struct SyncSlot<void> {
    bool done = false;
};

template <class T>
Task sync_wait_task(Async<T> a, SyncSlot<T>* slot) {
    if constexpr (std::is_void_v<T>) {
        co_await std::move(a);
    } else {
        slot->value.emplace(co_await std::move(a));
    }
    slot->done = true;
}

} // namespace detail

template <class T, class Pump>
T sync_wait(Async<T>&& a, Pump&& pump, std::uint8_t prio = 0) {
    detail::SyncSlot<T> slot;
    const bool started =
        kernel::start(detail::sync_wait_task<T>(std::move(a), &slot), prio);
    RTOS_ASSERT(started); // фрейм задачи-обёртки не выделился
    while (!slot.done) {
        pump();
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*slot.value);
    }
}

} // namespace rtos
