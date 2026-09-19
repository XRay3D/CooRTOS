// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

// coortos — кооперативный RTOS на stackless-корутинах C++26.
// Задачи с приоритетами, очереди, семафоры, мьютекс, sleep/таймауты,
// пробуждение из прерываний. Порт выбирается макросом:
// RTOS_PORT_CORTEX_M3 либо RTOS_PORT_HOST.

#include "config.hpp"      // IWYU pragma: export
#include "event_group.hpp" // IWYU pragma: export
#include "kernel.hpp"      // IWYU pragma: export
#include "mutex.hpp"      // IWYU pragma: export
#include "pool.hpp"       // IWYU pragma: export
#include "queue.hpp"      // IWYU pragma: export
#include "semaphore.hpp"  // IWYU pragma: export
#include "sync_wait.hpp"  // IWYU pragma: export
#include "task.hpp"       // IWYU pragma: export
#include "time.hpp"       // IWYU pragma: export
