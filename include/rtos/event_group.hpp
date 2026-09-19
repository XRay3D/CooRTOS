// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>
#include <cstdint>
#include <optional>

#include "kernel.hpp"

namespace rtos {

using EventBits = std::uint32_t;

// Группа событий (аналог FreeRTOS Event Groups): 32 бита-флага.
//
//  - co_await g.wait_any(bits) / wait_all(bits)        -> EventBits
//  - co_await g.wait_any_for(bits, t) / wait_all_for   -> optional<EventBits>
//  - co_await g.sync(set_bits, wait_bits)              -> EventBits (рандеву)
//  - set() / set_from_isr(), clear(), get()
//
// Семантика как в FreeRTOS: set() будит ВСЕХ ждущих, чьё условие
// выполнилось, и только после прохода по всем снимает биты, запрошенные
// ими через clear-on-exit — если двое ждут один бит, разбудятся оба.
// Результат ожидания — снимок битов в момент срабатывания условия.
class EventGroup {
public:
    constexpr EventGroup() = default;
    EventGroup(const EventGroup&) = delete;
    EventGroup& operator=(const EventGroup&) = delete;

    // Параметры и результат одного ожидания; на этот блок указывает
    // WaitNode::payload, из него set_locked() читает условие и в него же
    // кладёт снимок битов.
    struct WaitInfo {
        EventBits want;
        bool all;
        bool clear;
        EventBits result;
    };

    struct WaitAwaiter {
        EventGroup& g;
        WaitInfo info;
        WaitNode w;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (g.try_take_locked(info)) {
                return false;
            }
            w.payload = &info;
            kernel::block_current_locked(w, &g.waiters_, h);
            return true;
        }
        EventBits await_resume() noexcept { return info.result; }
    };

    struct WaitForAwaiter {
        EventGroup& g;
        WaitInfo info;
        Tick timeout;
        WaitNode w;
        bool got = false;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            if (g.try_take_locked(info)) {
                got = true;
                return false;
            }
            w.payload = &info;
            kernel::block_current_locked(w, &g.waiters_, h,
                                         /*has_deadline=*/true,
                                         kernel::now() + timeout);
            return true;
        }
        std::optional<EventBits> await_resume() noexcept {
            if (!got && w.timed_out) {
                return std::nullopt;
            }
            return info.result;
        }
    };

    // Рандеву (xEventGroupSync): атомарно выставить свои биты и дождаться
    // полного набора; завершивший набор участник проходит сразу, биты
    // рандеву снимаются. Условие проверяется по битам ДО clear-on-exit
    // разбуженных участников — иначе последний пришедший завис бы навсегда.
    struct SyncAwaiter {
        EventGroup& g;
        EventBits set_bits;
        WaitInfo info; // want=wait_bits, all=true, clear=true
        WaitNode w;

        bool await_ready() noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> h) {
            port::CriticalSection cs;
            const EventBits candidate = g.bits_ | set_bits;
            g.set_locked(set_bits); // будит остальных участников
            if ((candidate & info.want) == info.want) {
                info.result = candidate;
                g.bits_ &= ~info.want; // если set_locked ещё не снял
                return false;
            }
            w.payload = &info;
            kernel::block_current_locked(w, &g.waiters_, h);
            return true;
        }
        EventBits await_resume() noexcept { return info.result; }
    };

    [[nodiscard]] WaitAwaiter wait_any(EventBits bits,
                                       bool clear_on_exit = true) {
        return {*this, {bits, false, clear_on_exit, 0}, {}};
    }
    [[nodiscard]] WaitAwaiter wait_all(EventBits bits,
                                       bool clear_on_exit = true) {
        return {*this, {bits, true, clear_on_exit, 0}, {}};
    }
    [[nodiscard]] WaitForAwaiter wait_any_for(EventBits bits, Tick timeout,
                                              bool clear_on_exit = true) {
        return {*this, {bits, false, clear_on_exit, 0}, timeout, {}, false};
    }
    [[nodiscard]] WaitForAwaiter wait_all_for(EventBits bits, Tick timeout,
                                              bool clear_on_exit = true) {
        return {*this, {bits, true, clear_on_exit, 0}, timeout, {}, false};
    }
    template <class Rep, class Period>
    [[nodiscard]] WaitForAwaiter wait_any_for(EventBits bits,
                                              std::chrono::duration<Rep, Period> t,
                                              bool clear_on_exit = true) {
        return wait_any_for(bits, to_ticks(t), clear_on_exit);
    }
    template <class Rep, class Period>
    [[nodiscard]] WaitForAwaiter wait_all_for(EventBits bits,
                                              std::chrono::duration<Rep, Period> t,
                                              bool clear_on_exit = true) {
        return wait_all_for(bits, to_ticks(t), clear_on_exit);
    }
    [[nodiscard]] SyncAwaiter sync(EventBits set_bits, EventBits wait_bits) {
        return {*this, set_bits, {wait_bits, true, true, 0}, {}};
    }

    void set(EventBits bits) {
        port::CriticalSection cs;
        set_locked(bits);
    }
    void set_from_isr(EventBits bits) { set(bits); }

    // Снять биты; возвращает значение до снятия.
    EventBits clear(EventBits bits) {
        port::CriticalSection cs;
        const EventBits prev = bits_;
        bits_ &= ~bits;
        return prev;
    }

    EventBits get() const {
        port::CriticalSection cs;
        return bits_;
    }

private:
    static bool met(const WaitInfo& info, EventBits bits) {
        return info.all ? (bits & info.want) == info.want
                        : (bits & info.want) != 0;
    }

    // Условие выполнено прямо сейчас? Тогда забрать снимок и clear-on-exit.
    bool try_take_locked(WaitInfo& info) RTOS_PRE(port::in_critical()) {
        if (!met(info, bits_)) {
            return false;
        }
        info.result = bits_;
        if (info.clear) {
            bits_ &= ~info.want;
        }
        return true;
    }

    void set_locked(EventBits bits) RTOS_PRE(port::in_critical()) {
        bits_ |= bits;
        EventBits to_clear = 0;
        for (ListNode* n = waiters_.head.next; n != &waiters_.head;) {
            ListNode* next = n->next; // wake_locked снимет узел со списка
            WaitNode* w = static_cast<WaitNode*>(n->owner);
            WaitInfo* info = static_cast<WaitInfo*>(w->payload);
            if (met(*info, bits_)) {
                info->result = bits_;
                if (info->clear) {
                    to_clear |= info->want;
                }
                kernel::wake_locked(w);
            }
            n = next;
        }
        bits_ &= ~to_clear;
    }

    EventBits bits_ = 0;
    List waiters_;
};

} // namespace rtos
