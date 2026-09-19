// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Host-порт (x86, юнит-тесты): однопоточная модель, "критическая секция" —
// счётчик вложенности с проверкой баланса; вместо WFI — idle_hook.

namespace rtos::port {

inline int critical_depth = 0;

// --- Эмуляция PendSV для RTOS_PREEMPTIVE_WAKE ---
// pend_preempt() взводит флаг; «обработчик» (указатель ставит kernel)
// срабатывает на выходе из внешней критической секции — как PendSV,
// который ждёт разрешения прерываний.
inline bool preempt_pending = false;
inline void (*preempt_dispatch)() = nullptr;

inline void pend_preempt() { preempt_pending = true; }
inline void preempt_init() {}

[[noreturn]] inline void trap() {
    std::fprintf(stderr, "rtos: trap\n");
    std::abort();
}

inline void irq_disable() { ++critical_depth; }

inline void irq_enable() {
    if (critical_depth <= 0) {
        std::fprintf(stderr, "rtos: unbalanced irq_enable\n");
        std::abort();
    }
    --critical_depth;
    if (critical_depth == 0 && preempt_pending && preempt_dispatch) {
        preempt_dispatch();
    }
}

inline bool in_critical() { return critical_depth > 0; }

inline std::uint32_t irq_save() {
    irq_disable();
    return 0;
}

inline void irq_restore(std::uint32_t) { irq_enable(); }

struct CriticalSection {
    CriticalSection() { irq_disable(); }
    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
    ~CriticalSection() { irq_enable(); }
};

// Тесты обычно гоняют run_one()/tick_isr() вручную и до idle() не доходят;
// сработавший без хука idle означает дедлок.
inline void (*idle_hook)() = nullptr;

inline void idle() {
    if (idle_hook) {
        idle_hook();
    } else {
        std::fprintf(stderr, "rtos: idle with no pending work (deadlock)\n");
        std::abort();
    }
}

} // namespace rtos::port
