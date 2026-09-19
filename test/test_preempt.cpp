// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

// Тесты RTOS_PREEMPTIVE_WAKE (собираются только в rtos_preempt_tests).
// «ISR» — вызов *_from_isr из тела выполняющейся задачи: pend взводится
// в критсекции примитива и срабатывает на выходе из неё, как настоящий
// PendSV при разрешении прерываний.

#include <vector>

#include "test_util.hpp"

using namespace rtos;

static Task waiter(BinarySemaphore* s, std::vector<int>* log, int mark) {
    co_await s->acquire();
    log->push_back(mark);
}

static Task releaser(BinarySemaphore* s, std::vector<int>* log) {
    log->push_back(1);
    s->release_from_isr(); // «прерывание» посреди задачи
    log->push_back(2);
    co_await kernel::yield();
    log->push_back(3);
}

RTOS_TEST(preempt_higher_runs_inside_lower) {
    BinarySemaphore s;
    std::vector<int> log;
    kernel::start(waiter(&s, &log, 100), 0);
    CHECK(run_until_idle());
    kernel::start(releaser(&s, &log), 2);
    CHECK(run_until_idle());
    // 100 вклинивается сразу после release, до продолжения releaser'а
    CHECK((log == std::vector<int>{1, 100, 2, 3}));
}

RTOS_TEST(preempt_not_equal_or_lower) {
    BinarySemaphore s_eq, s_lo;
    std::vector<int> log;
    kernel::start(waiter(&s_eq, &log, 200), 1); // равный приоритет
    CHECK(run_until_idle());
    kernel::start(releaser(&s_eq, &log), 1);
    CHECK(run_until_idle());
    // Вытеснения нет (200 не между 1 и 2); равный приоритет дожидается
    // co_await, а yield ставит releaser в хвост за разбуженным waiter'ом.
    CHECK((log == std::vector<int>{1, 2, 200, 3}));

    log.clear();
    kernel::start(waiter(&s_lo, &log, 300), 3); // ниже приоритетом
    CHECK(run_until_idle());
    kernel::start(releaser(&s_lo, &log), 1);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 2, 3, 300}));
}

static Task mid_task(BinarySemaphore* sm, BinarySemaphore* sh,
                     std::vector<int>* log) {
    co_await sm->acquire();
    log->push_back(20);
    sh->release_from_isr(); // будит high, находясь внутри вытеснения
    log->push_back(21);
}

static Task low_task(BinarySemaphore* sm, std::vector<int>* log) {
    log->push_back(1);
    sm->release_from_isr();
    log->push_back(2);
    co_return;
}

RTOS_TEST(preempt_nested_like_pendsv) {
    // PendSV не вытесняет сам себя: high, разбуженный внутри mid,
    // выполняется после блокировки mid повторным проходом цикла,
    // но всё ещё до продолжения low.
    BinarySemaphore sm, sh;
    std::vector<int> log;
    kernel::start(waiter(&sh, &log, 100), 0);
    kernel::start(mid_task(&sm, &sh, &log), 1);
    CHECK(run_until_idle());
    kernel::start(low_task(&sm, &log), 2);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 20, 21, 100, 2}));
}

static Task q_receiver(Queue<int, 4>* q, std::vector<int>* log) {
    log->push_back(co_await q->receive());
}

static Task q_sender(Queue<int, 4>* q, std::vector<int>* log) {
    log->push_back(1);
    q->try_send_from_isr(42); // direct handoff + вытеснение
    log->push_back(2);
    co_return;
}

RTOS_TEST(preempt_queue_direct_handoff) {
    Queue<int, 4> q;
    std::vector<int> log;
    kernel::start(q_receiver(&q, &log), 0);
    CHECK(run_until_idle());
    kernel::start(q_sender(&q, &log), 2);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{1, 42, 2}));
}

static Task bump_task(int* v) {
    *v += 1;
    co_return;
}

RTOS_TEST(preempt_not_at_idle_start) {
    // start() вне работающей задачи не должен исполнять задачу немедленно
    int v = 0;
    CHECK(kernel::start(bump_task(&v), 0));
    CHECK(v == 0);
    CHECK(run_until_idle());
    CHECK(v == 1);
}
