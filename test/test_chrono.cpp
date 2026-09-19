// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

// co_await <chrono-длительность> и chrono-перегрузки таймаутов по всему API.

#include <chrono>
#include <vector>

#include "test_util.hpp"

using namespace rtos;
using namespace std::chrono_literals;

static Task chrono_sleeper(std::vector<int>* log, int id) {
    co_await 3ms; // == co_await sleep_for(3)
    log->push_back(id);
}

RTOS_TEST(chrono_await_duration_sleeps) {
    std::vector<int> log;
    kernel::start(chrono_sleeper(&log, 1), 0);
    CHECK(run_until_idle());
    advance_ticks(2);
    CHECK(log.empty());
    advance_ticks(1);
    CHECK((log == std::vector<int>{1}));
}

static Task subtick(std::vector<int>* log) {
    co_await 1500us; // округление вверх -> 2 тика
    log->push_back(1);
    co_await 0ms; // не блокируется
    log->push_back(2);
    co_await sleep_for(1ms); // перегрузка sleep_for(duration)
    log->push_back(3);
}

RTOS_TEST(chrono_subtick_rounds_up) {
    std::vector<int> log;
    kernel::start(subtick(&log), 0);
    CHECK(run_until_idle());
    advance_ticks(1);
    CHECK(log.empty());
    advance_ticks(1);
    CHECK((log == std::vector<int>{1, 2}));
    advance_ticks(1);
    CHECK((log == std::vector<int>{1, 2, 3}));
}

static Async<void> nested_pause() { co_await 2ms; }

static Task nested_chrono(std::vector<int>* log) {
    co_await nested_pause(); // await_transform и во вложенной корутине
    log->push_back(7);
}

RTOS_TEST(chrono_in_nested_async) {
    std::vector<int> log;
    kernel::start(nested_chrono(&log), 0);
    CHECK(run_until_idle());
    advance_ticks(1);
    CHECK(log.empty());
    advance_ticks(1);
    CHECK((log == std::vector<int>{7}));
}

static Task api_timeouts(Queue<int, 4>* q, CountingSemaphore* s, EventGroup* g,
                         std::vector<int>* log) {
    auto r = co_await q->receive_for(2ms);
    log->push_back(r ? *r : -1);
    log->push_back(co_await s->try_acquire_for(1ms) ? 1 : 0);
    auto e = co_await g->wait_any_for(0x1, 1ms);
    log->push_back(e ? static_cast<int>(*e) : -2);
    log->push_back(co_await q->send_for(9, 1ms) ? 1 : 0);
}

RTOS_TEST(chrono_timeout_overloads_across_api) {
    Queue<int, 4> q;
    CountingSemaphore s(0, 1);
    EventGroup g;
    std::vector<int> log;
    kernel::start(api_timeouts(&q, &s, &g, &log), 0);
    CHECK(run_until_idle());
    advance_ticks(5); // все таймауты истекают, send_for(9) успевает в буфер
    CHECK((log == std::vector<int>{-1, 0, -2, 1}));
    int v = 0;
    CHECK(q.try_receive(v) && v == 9);
}

RTOS_TEST(chrono_to_ticks_conversions) {
    CHECK(to_ticks(1s) == 1000u);
    CHECK(to_ticks(1ms) == 1u);
    CHECK(to_ticks(999us) == 1u);  // вверх
    CHECK(to_ticks(1001us) == 2u); // вверх
    CHECK(to_ticks(0s) == 0u);
    CHECK(to_ticks(-5ms) == 0u);
    CHECK(to_ticks(2min) == 120000u);
}
