// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Task sleeper(std::vector<int>* log, int id, Tick t) {
    co_await sleep_for(t);
    log->push_back(id);
}

RTOS_TEST(sleep_wakes_in_deadline_order) {
    std::vector<int> log;
    kernel::start(sleeper(&log, 3, 3), 0);
    kernel::start(sleeper(&log, 1, 1), 0);
    kernel::start(sleeper(&log, 2, 2), 0);
    CHECK(run_until_idle());
    CHECK(log.empty());
    advance_ticks(1);
    CHECK((log == std::vector<int>{1}));
    advance_ticks(1);
    CHECK((log == std::vector<int>{1, 2}));
    advance_ticks(1);
    CHECK((log == std::vector<int>{1, 2, 3}));
}

RTOS_TEST(sleep_zero_does_not_block) {
    std::vector<int> log;
    kernel::start(sleeper(&log, 7, 0), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{7}));
}

RTOS_TEST(tick_count_wraparound) {
    kernel::tick_count = 0xFFFFFFF0u;
    std::vector<int> log;
    kernel::start(sleeper(&log, 1, 0x08), 0); // дедлайн до переполнения
    kernel::start(sleeper(&log, 2, 0x20), 0); // дедлайн после переполнения
    CHECK(run_until_idle());
    advance_ticks(0x08);
    CHECK((log == std::vector<int>{1}));
    advance_ticks(0x18);
    CHECK((log == std::vector<int>{1, 2}));
    CHECK(kernel::now() == 0x10u);
}
