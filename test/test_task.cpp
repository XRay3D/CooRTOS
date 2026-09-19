// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Task bump(int* v) {
    *v += 1;
    co_return;
}

RTOS_TEST(task_runs_and_completes) {
    int v = 0;
    CHECK(kernel::start(bump(&v), 0));
    CHECK(v == 0); // initial_suspend: до run() тело не выполняется
    CHECK(run_until_idle());
    CHECK(v == 1);
}

static Task logger(std::vector<int>* log, int id) {
    log->push_back(id);
    co_return;
}

RTOS_TEST(priority_order) {
    std::vector<int> log;
    kernel::start(logger(&log, 2), 2);
    kernel::start(logger(&log, 0), 0);
    kernel::start(logger(&log, 1), 1);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{0, 1, 2}));
}

static Task yielding(std::vector<int>* log, int id, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        log->push_back(id);
        co_await kernel::yield();
    }
}

RTOS_TEST(same_priority_round_robin) {
    std::vector<int> log;
    kernel::start(yielding(&log, 1, 3), 1);
    kernel::start(yielding(&log, 2, 3), 1);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 2, 1, 2, 1, 2}));
}

RTOS_TEST(higher_priority_preempts_at_yield) {
    // Низкоприоритетная уступает — высокоприоритетная, разбуженная тиком,
    // выполняется раньше её продолжения.
    std::vector<int> log;
    kernel::start(yielding(&log, 9, 2), 3);
    kernel::start(logger(&log, 0), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{0, 9, 9}));
}

// --- Реентерабельность run_one(): задача синхронно зовёт код, который сам
// крутит планировщик (главный контекст приложения внутри своего ожидания).

static Task nested_worker(std::vector<int>* log) {
    log->push_back(2);
    co_await sleep_for(1);
    log->push_back(3);
}

// Обычная (не корутинная) функция: качает планировщик до опустошения.
static void pump_from_plain_code() {
    while (kernel::run_one()) {
    }
}

static Task outer_caller(std::vector<int>* log) {
    log->push_back(1);
    pump_from_plain_code(); // внутри исполняется nested_worker до его co_await
    log->push_back(4);
    co_await sleep_for(1); // current должен снова указывать на эту задачу
    log->push_back(5);
}

RTOS_TEST(run_one_nested_from_task) {
    std::vector<int> log;
    kernel::start(outer_caller(&log), 1);
    kernel::start(nested_worker(&log), 2); // ниже приоритетом: до вложенного pump не дойдёт
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 2, 4}));
    CHECK(kernel::current == nullptr);
    CHECK(kernel::running_prio == kernel::NoRunningTask);
    advance_ticks(1); // обе просыпаются одним тиком; первой — приоритетнее (outer)
    CHECK((log == std::vector<int>{1, 2, 4, 5, 3}));
}
