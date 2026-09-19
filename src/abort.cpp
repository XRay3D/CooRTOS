// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

// Подмена newlib'овского abort() для прошивок: std::terminate() (контракты
// quick_enforce, чисто-виртуальные вызовы и т.п.) завершается коротким
// трапом с запрещёнными прерываниями вместо втягивания raise/signal-механики
// newlib. Экономит ~1К flash и делает точку смерти видимой отладчику (udf).
//
// Объектник добавляется в каждую прошивку опцией CMake RTOS_PROVIDE_ABORT
// (по умолчанию ON); объект в составе executable всегда перекрывает
// одноимённый символ из архива libc.

#include <rtos/port.hpp>

extern "C" [[noreturn]] void abort() { rtos::port::trap(); }
