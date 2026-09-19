// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Task waiter(CountingSemaphore* s, std::vector<int>* log, int id) {
    co_await s->acquire();
    log->push_back(id);
}

RTOS_TEST(semaphore_counting_and_fifo) {
    CountingSemaphore s(2, 2);
    CHECK(s.try_acquire());
    CHECK(s.try_acquire());
    CHECK(!s.try_acquire()); // счётчик исчерпан

    std::vector<int> log;
    kernel::start(waiter(&s, &log, 1), 0);
    kernel::start(waiter(&s, &log, 2), 0);
    CHECK(run_until_idle());
    CHECK(log.empty());

    CHECK(s.release()); // будит первого (FIFO), счётчик не растёт
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1}));
    CHECK(!s.try_acquire()); // handoff: значение не украсть

    CHECK(s.release());
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 2}));

    CHECK(s.release());
    CHECK(s.release());
    CHECK(!s.release()); // выше max — потерян
    CHECK(s.count() == 2);
    CHECK(s.try_acquire() && s.try_acquire());
}

RTOS_TEST(semaphore_release_from_isr) {
    BinarySemaphore s;
    std::vector<int> log;
    kernel::start(waiter(&s, &log, 5), 0);
    CHECK(run_until_idle());
    CHECK(log.empty());
    CHECK(s.release_from_isr());
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{5}));
}

static Task timed_waiter(CountingSemaphore* s, std::vector<int>* log,
                         Tick timeout) {
    bool ok = co_await s->try_acquire_for(timeout);
    log->push_back(ok ? 1 : 0);
}

RTOS_TEST(semaphore_timeout) {
    BinarySemaphore s;
    std::vector<int> log;
    kernel::start(timed_waiter(&s, &log, 3), 0);
    CHECK(run_until_idle());
    advance_ticks(3);
    CHECK((log == std::vector<int>{0})); // таймаут

    kernel::start(timed_waiter(&s, &log, 3), 0);
    CHECK(run_until_idle());
    advance_ticks(2);
    CHECK(s.release_from_isr()); // событие раньше дедлайна
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{0, 1}));
    advance_ticks(3); // дедлайн не срабатывает вдогонку
    CHECK((log == std::vector<int>{0, 1}));
}
