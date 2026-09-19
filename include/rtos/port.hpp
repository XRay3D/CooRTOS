// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

// RTOS_PORT_CORTEX_M3 — устаревший синоним RTOS_PORT_CORTEX_M
#if defined(RTOS_PORT_CORTEX_M3) && !defined(RTOS_PORT_CORTEX_M)
#define RTOS_PORT_CORTEX_M
#endif

#if defined(RTOS_PORT_CORTEX_M)
#include "port/cortex_m.hpp"
#elif defined(RTOS_PORT_RISCV32)
#include "port/riscv32.hpp"
#elif defined(RTOS_PORT_HOST)
#include "port/host.hpp"
#else
#error "Define RTOS_PORT_CORTEX_M, RTOS_PORT_RISCV32 or RTOS_PORT_HOST"
#endif
