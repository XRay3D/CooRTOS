// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

// Собирается отдельным бинарником: пул (без RTOS_USE_MALLOC),
// RTOS_FRAME_BLOCK_COUNT=2.

#include "test_util.hpp"

using namespace rtos;

static Task blocked_task(BinarySemaphore* s) { co_await s->acquire(); }

static std::size_t g_failed_size = 0;

RTOS_TEST(pool_exhaustion_hook) {
    config::alloc_failed_hook = [](std::size_t n) { g_failed_size = n; };
    BinarySemaphore s;
    {
        Task t1 = blocked_task(&s);
        Task t2 = blocked_task(&s);
        CHECK(t1 && t2);
        CHECK(frame_pool.free_count() == 0);
        Task t3 = blocked_task(&s); // пул пуст
        CHECK(!t3);
        CHECK(g_failed_size > 0);
    } // незапущенные задачи уничтожены — блоки вернулись
    CHECK(frame_pool.free_count() == 2);
    config::alloc_failed_hook = nullptr;
}

RTOS_TEST(pool_block_reuse_through_lifecycle) {
    BinarySemaphore s;
    for (int i = 0; i < 5; ++i) {
        CHECK(kernel::start(blocked_task(&s), 0));
        CHECK(run_until_idle());
        CHECK(frame_pool.free_count() == 1); // задача жива и блокирована
        CHECK(s.release());
        CHECK(run_until_idle()); // задача завершилась, фрейм освобождён
        CHECK(frame_pool.free_count() == 2);
    }
}

static Task oversized_frame(int* out) {
    volatile char big[RTOS_FRAME_BLOCK_SIZE * 2] = {};
    big[0] = 1;
    *out = big[0];
    co_await kernel::yield();
    *out = big[0] + 1;
}

RTOS_TEST(pool_oversized_frame_hits_hook) {
    g_failed_size = 0;
    config::alloc_failed_hook = [](std::size_t n) { g_failed_size = n; };
    int out = 0;
    Task t = oversized_frame(&out);
    CHECK(!t);
    CHECK(g_failed_size > RTOS_FRAME_BLOCK_SIZE);
    CHECK(frame_pool.free_count() == 2);
    config::alloc_failed_hook = nullptr;
}
