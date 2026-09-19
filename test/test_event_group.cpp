// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Task wait_any_task(EventGroup* g, EventBits bits, bool clear,
                          std::vector<EventBits>* log) {
    log->push_back(co_await g->wait_any(bits, clear));
}

static Task wait_all_task(EventGroup* g, EventBits bits, bool clear,
                          std::vector<EventBits>* log) {
    log->push_back(co_await g->wait_all(bits, clear));
}

RTOS_TEST(event_wait_any_vs_all) {
    EventGroup g;
    std::vector<EventBits> any_log, all_log;
    kernel::start(wait_any_task(&g, 0x3, false, &any_log), 0);
    kernel::start(wait_all_task(&g, 0x3, false, &all_log), 0);
    CHECK(run_until_idle());

    g.set(0x1);
    CHECK(run_until_idle());
    CHECK((any_log == std::vector<EventBits>{0x1})); // any — по первому биту
    CHECK(all_log.empty());                          // all ждёт оба

    g.set(0x2);
    CHECK(run_until_idle());
    CHECK((all_log == std::vector<EventBits>{0x3}));
    CHECK(g.get() == 0x3); // clear-on-exit не запрашивали
    g.clear(0x3);
}

RTOS_TEST(event_clear_on_exit_wakes_all_first) {
    // Как в FreeRTOS: оба ждущих одного бита будятся, clear — после прохода
    EventGroup g;
    std::vector<EventBits> log1, log2;
    kernel::start(wait_any_task(&g, 0x4, true, &log1), 0);
    kernel::start(wait_any_task(&g, 0x4, true, &log2), 0);
    CHECK(run_until_idle());
    g.set(0x4);
    CHECK(run_until_idle());
    CHECK((log1 == std::vector<EventBits>{0x4}));
    CHECK((log2 == std::vector<EventBits>{0x4}));
    CHECK(g.get() == 0); // снят после пробуждения обоих
}

RTOS_TEST(event_ready_immediately) {
    EventGroup g;
    g.set(0x9);
    std::vector<EventBits> log;
    kernel::start(wait_all_task(&g, 0x9, true, &log), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<EventBits>{0x9})); // без блокировки
    CHECK(g.get() == 0);
}

static Task wait_for_task(EventGroup* g, EventBits bits, Tick t,
                          std::vector<int>* log) {
    auto r = co_await g->wait_any_for(bits, t);
    log->push_back(r ? static_cast<int>(*r) : -1);
}

RTOS_TEST(event_timeout) {
    EventGroup g;
    std::vector<int> log;
    kernel::start(wait_for_task(&g, 0x1, 3, &log), 0);
    CHECK(run_until_idle());
    advance_ticks(3);
    CHECK((log == std::vector<int>{-1}));

    g.set(0x1); // поздний set не будит «мёртвого» ждущего, бит остаётся
    CHECK(run_until_idle());
    CHECK(g.get() == 0x1);
    g.clear(0x1);

    kernel::start(wait_for_task(&g, 0x2, 5, &log), 0);
    CHECK(run_until_idle());
    advance_ticks(2);
    g.set_from_isr(0x2); // событие раньше дедлайна (ISR-путь)
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{-1, 0x2}));
    advance_ticks(5); // дедлайн не срабатывает вдогонку
    CHECK((log == std::vector<int>{-1, 0x2}));
}

static Task sync_worker(EventGroup* g, EventBits my_bit,
                        std::vector<EventBits>* log) {
    log->push_back(co_await g->sync(my_bit, 0x7));
}

RTOS_TEST(event_sync_rendezvous) {
    EventGroup g;
    std::vector<EventBits> log;
    kernel::start(sync_worker(&g, 0x1, &log), 0);
    CHECK(run_until_idle());
    kernel::start(sync_worker(&g, 0x2, &log), 0);
    CHECK(run_until_idle());
    CHECK(log.empty()); // двое ждут третьего

    kernel::start(sync_worker(&g, 0x4, &log), 0);
    CHECK(run_until_idle());
    // Все трое прошли (включая замкнувшего рандеву), биты сняты
    CHECK((log == std::vector<EventBits>{0x7, 0x7, 0x7}));
    CHECK(g.get() == 0);
}
