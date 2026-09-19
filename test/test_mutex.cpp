// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Task locker(Mutex* m, int* inside, std::vector<int>* log, int id) {
    co_await m->lock();
    CHECK(*inside == 0); // взаимное исключение
    ++*inside;
    log->push_back(id);
    co_await kernel::yield(); // удерживаем мьютекс через переключение
    co_await kernel::yield();
    --*inside;
    m->unlock();
}

RTOS_TEST(mutex_exclusion_and_handoff) {
    Mutex m;
    int inside = 0;
    std::vector<int> log;
    kernel::start(locker(&m, &inside, &log, 1), 0);
    kernel::start(locker(&m, &inside, &log, 2), 0);
    kernel::start(locker(&m, &inside, &log, 3), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 2, 3})); // FIFO handoff
    CHECK(inside == 0);
    CHECK(!m.locked());
}

static Task try_lock_script(Mutex* m) {
    CHECK(m->try_lock());
    CHECK(!m->try_lock()); // не рекурсивный
    m->unlock();
    CHECK(!m->locked());
    co_return;
}

RTOS_TEST(mutex_try_lock) {
    Mutex m;
    kernel::start(try_lock_script(&m), 0);
    CHECK(run_until_idle());
    CHECK(!m.locked());
}
