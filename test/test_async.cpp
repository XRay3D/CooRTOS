// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Async<int> inner_blocking(CountingSemaphore* s) {
    co_await s->acquire(); // блокируется ВНУТРИ вложенной корутины
    co_return 123;
}

static Task outer(CountingSemaphore* s, std::vector<int>* log) {
    int v = co_await inner_blocking(s);
    log->push_back(v);
}

RTOS_TEST(nested_async_blocks_inner) {
    BinarySemaphore s;
    std::vector<int> log;
    kernel::start(outer(&s, &log), 0);
    CHECK(run_until_idle());
    CHECK(log.empty()); // заблокирована во вложенном фрейме
    CHECK(s.release());
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{123})); // значение прошло symmetric transfer
}

static Async<int> add_async(int a, int b) { co_return a + b; }

static Async<int> sum_via_two_levels(int a, int b, int c) {
    int ab = co_await add_async(a, b);
    co_return ab + c;
}

static Task compute(std::vector<int>* log) {
    log->push_back(co_await sum_via_two_levels(1, 2, 3));
}

RTOS_TEST(nested_async_two_levels_no_blocking) {
    std::vector<int> log;
    kernel::start(compute(&log), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{6}));
}

static Async<void> tick_pause() { co_await sleep_for(2); }

static Task pauser(std::vector<int>* log) {
    co_await tick_pause();
    log->push_back(77);
}

RTOS_TEST(nested_async_void_sleep) {
    std::vector<int> log;
    kernel::start(pauser(&log), 0);
    CHECK(run_until_idle());
    CHECK(log.empty());
    advance_ticks(2);
    CHECK((log == std::vector<int>{77}));
}

// --- sync_wait: Async из не-корутинного контекста ---

static Async<int> waits_for_semaphore(CountingSemaphore* s) {
    co_await s->acquire();
    co_return 5;
}

RTOS_TEST(sync_wait_runs_async_to_completion) {
    BinarySemaphore s;
    int pumps = 0;
    const int v = sync_wait(waits_for_semaphore(&s), [&] {
        ++pumps;
        if (pumps == 3) {
            CHECK(s.release()); // «прерывание» приходит на третьем обороте
        }
        CHECK(run_until_idle());
    });
    CHECK(v == 5);
    CHECK(pumps >= 3);
    CHECK(kernel::current == nullptr);
}

static Async<void> sleeps_two_ticks(std::vector<int>* log) {
    co_await sleep_for(2);
    log->push_back(9);
}

RTOS_TEST(sync_wait_void_with_ticks) {
    std::vector<int> log;
    int ticks = 0;
    sync_wait(sleeps_two_ticks(&log), [&] {
        ++ticks;
        advance_ticks(1);
    });
    CHECK((log == std::vector<int>{9}));
    // первый pump лишь запускает обёртку (она засыпает на тике 1 до тика 3)
    CHECK(ticks == 3);
}

// sync_wait изнутри задачи: вызывающая задача Running, вложенная Async
// исполняется другой задачей-обёрткой поверх её стека.
static Async<int> quick_value() { co_return 11; }

static Task task_using_sync_wait(std::vector<int>* log) {
    const int v = sync_wait(quick_value(), [] { CHECK(run_until_idle()); });
    log->push_back(v);
    co_await sleep_for(1); // после вложенного run_one() блокировка корректна
    log->push_back(12);
}

RTOS_TEST(sync_wait_from_inside_task) {
    std::vector<int> log;
    kernel::start(task_using_sync_wait(&log), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{11}));
    advance_ticks(1);
    CHECK((log == std::vector<int>{11, 12}));
}
