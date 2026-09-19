// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <cstddef>
#include <cstdint>

#include "port.hpp"

namespace rtos {
// Единица времени ядра: тики системного таймера (при RTOS_TICK_HZ=1000 —
// миллисекунды). 32 бита => максимальный интервал ~49.7 суток при 1 кГц.
using Tick = std::uint32_t;
} // namespace rtos

// Частота тика (Гц): должна совпадать с настройкой системного таймера
// (board::systick_init и т.п.). Используется для конверсии
// std::chrono-длительностей в тики.
#ifndef RTOS_TICK_HZ
#define RTOS_TICK_HZ 1000
#endif

// Тюнинги ядра. Переопределяются через target_compile_definitions
// до включения заголовков rtos.

#ifndef RTOS_NUM_PRIORITIES
#define RTOS_NUM_PRIORITIES 4 // 0 — высший приоритет
#endif

// Размер и число блоков статического пула под фреймы корутин
// (используется, если не задан RTOS_USE_MALLOC).
#ifndef RTOS_FRAME_BLOCK_SIZE
#define RTOS_FRAME_BLOCK_SIZE 256
#endif

#ifndef RTOS_FRAME_BLOCK_COUNT
#define RTOS_FRAME_BLOCK_COUNT 8
#endif

#define RTOS_TRAP() ::rtos::port::trap()

// Всегда включённая жёсткая проверка (трап). Для проверок, которые обязаны
// работать даже в сборке без контрактов и с семантикой ignore.
#define RTOS_ASSERT(cond)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            RTOS_TRAP();                                                       \
        }                                                                      \
    } while (false)

// --- Контракты C++26 (P2900) -----------------------------------------------
// Протокол ядра описан контрактами: RTOS_PRE/RTOS_POST на сигнатурах,
// RTOS_CONTRACT_ASSERT в телах. Семантика (ignore/observe/enforce/
// quick_enforce) выбирается флагом компилятора на этапе сборки — см.
// корневой CMakeLists.txt.
//
// Заглушки при отключении: если компилятор не знает контрактов
// (__cpp_contracts не определён) либо задан RTOS_NO_CONTRACTS,
// RTOS_PRE/RTOS_POST исчезают, а RTOS_CONTRACT_ASSERT деградирует до
// всегда включённого RTOS_ASSERT (трап) — проверки не теряются молча.
#if defined(__cpp_contracts) && !defined(RTOS_NO_CONTRACTS)
#define RTOS_CONTRACTS_ENABLED 1
#define RTOS_PRE(...) pre(__VA_ARGS__)
#define RTOS_POST(...) post(__VA_ARGS__)
#define RTOS_CONTRACT_ASSERT(...) contract_assert(__VA_ARGS__)
#else
#define RTOS_CONTRACTS_ENABLED 0
#define RTOS_PRE(...)
#define RTOS_POST(...)
#define RTOS_CONTRACT_ASSERT(...) RTOS_ASSERT(__VA_ARGS__)
#endif

namespace rtos::config {

// Вызывается при нехватке памяти под фрейм корутины (пул пуст или фрейм
// больше блока; `requested` — реальный размер фрейма). nullptr — трап.
// Если хук установлен и вернул управление, аллокация вернёт nullptr и
// соответствующий Task окажется невалидным (operator bool == false).
inline void (*alloc_failed_hook)(std::size_t requested) = nullptr;

} // namespace rtos::config
