// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <cstdint>

// Порт Cortex-M — любое ядро семейства (M0/M0+/M3/M4/M7/M23/M33...):
// используется только PRIMASK, доступный во всех ARMv6-M/v7-M/v8-M
// (BASEPRI не нужен — блокировка ядра глобальная), и WFI в простое.
// Без зависимости от CMSIS — только inline asm.

namespace rtos::port {

inline std::uint32_t irq_save() {
    std::uint32_t pm;
    asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(pm)::"memory");
    return pm;
}

inline void irq_restore(std::uint32_t pm) {
    asm volatile("msr primask, %0" ::"r"(pm) : "memory");
}

inline void irq_disable() { asm volatile("cpsid i" ::: "memory"); }
inline void irq_enable() { asm volatile("cpsie i" ::: "memory"); }

// Для контрактов *_locked-функций: прерывания запрещены (PRIMASK = 1)?
inline bool in_critical() {
    std::uint32_t pm;
    asm("mrs %0, primask" : "=r"(pm));
    return (pm & 1u) != 0;
}

struct CriticalSection {
    std::uint32_t pm = irq_save();
    CriticalSection() = default;
    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;
    ~CriticalSection() { irq_restore(pm); }
};

// Вызывается планировщиком с запрещёнными прерываниями: WFI просыпается
// от pending-прерывания даже при PRIMASK=1, обработчик выполнится после
// последующего irq_enable().
inline void idle() { asm volatile("wfi"); }

// --- Вытесняющее пробуждение (RTOS_PREEMPTIVE_WAKE) через PendSV ---

// PendSV — низший приоритет прерываний (SHPR3, word-доступ: на ARMv6-M
// байтовый доступ к SCS непредсказуем). Вызывается из kernel::run().
inline void preempt_init() {
    auto& shpr3 = *reinterpret_cast<volatile std::uint32_t*>(0xE000ED20u);
    shpr3 = shpr3 | 0x00FF0000u;
}

// Запросить PendSV (ICSR.PENDSVSET); сработает, когда прерывания разрешены
// и активные обработчики завершатся.
inline void pend_preempt() {
    *reinterpret_cast<volatile std::uint32_t*>(0xE000ED04u) = 1u << 28;
}

[[noreturn]] inline void trap() {
    irq_disable();
    __builtin_trap();
}

} // namespace rtos::port
