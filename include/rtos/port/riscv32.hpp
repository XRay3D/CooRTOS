// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <cstdint>

// Порт RV32 (rv32imc_zicsr_zifencei и совместимые, напр. MIK32 Амур):
// код в machine mode, критические секции через mstatus.MIE, WFI в простое.
// Требуется расширение Zicsr. Источник тика (machine timer и т.п.)
// настраивает приложение и зовёт rtos::kernel::tick_isr() из обработчика.

#ifdef RTOS_PREEMPTIVE_WAKE
#error "RTOS_PREEMPTIVE_WAKE не поддержан на RISCV32: нужен software \
interrupt конкретной платформы (CLINT MSIP и т.п.)"
#endif

namespace rtos::port {

inline constexpr std::uint32_t MstatusMie = 0x8; // бит 3

// Атомарно запрещает прерывания, возвращает прежний mstatus.
inline std::uint32_t irq_save() {
    std::uint32_t prev;
    asm volatile("csrrci %0, mstatus, %1"
                 : "=r"(prev)
                 : "i"(MstatusMie)
                 : "memory");
    return prev;
}

inline void irq_restore(std::uint32_t prev) {
    if (prev & MstatusMie) {
        asm volatile("csrsi mstatus, %0" ::"i"(MstatusMie) : "memory");
    }
}

inline void irq_disable() {
    asm volatile("csrci mstatus, %0" ::"i"(MstatusMie) : "memory");
}

inline void irq_enable() {
    asm volatile("csrsi mstatus, %0" ::"i"(MstatusMie) : "memory");
}

// Для контрактов *_locked-функций: прерывания запрещены (MIE = 0)?
inline bool in_critical() {
    std::uint32_t mstatus;
    asm("csrr %0, mstatus" : "=r"(mstatus));
    return (mstatus & MstatusMie) == 0;
}

struct CriticalSection {
    std::uint32_t prev = irq_save();
    CriticalSection() = default;
    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
    ~CriticalSection() { irq_restore(prev); }
};

// Вызывается планировщиком с запрещёнными прерываниями: по спецификации
// RISC-V WFI просыпается от pending-прерывания и при MIE=0 (либо
// реализована как NOP — тогда цикл планировщика просто крутится),
// обработчик выполнится после последующего irq_enable().
inline void idle() { asm volatile("wfi"); }

[[noreturn]] inline void trap() {
    irq_disable();
    __builtin_trap(); // ebreak
}

} // namespace rtos::port
