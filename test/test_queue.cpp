// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include <vector>

#include "test_util.hpp"

using namespace rtos;

using IntQueue = Queue<int, 4>;

RTOS_TEST(queue_fifo_no_block) {
    IntQueue q;
    CHECK(q.try_send(1));
    CHECK(q.try_send(2));
    CHECK(q.try_send(3));
    CHECK(q.size() == 3);
    int v = 0;
    CHECK(q.try_receive(v) && v == 1);
    CHECK(q.try_receive(v) && v == 2);
    CHECK(q.try_receive(v) && v == 3);
    CHECK(!q.try_receive(v));
}

static Task consumer(IntQueue* q, std::vector<int>* log) {
    log->push_back(co_await q->receive());
}

static Task producer(IntQueue* q, int v) { co_await q->send(v); }

static Task producer2(Queue<int, 2>* q, int v) { co_await q->send(v); }

RTOS_TEST(queue_recv_blocks_then_wakes) {
    IntQueue q;
    std::vector<int> log;
    kernel::start(consumer(&q, &log), 0);
    CHECK(run_until_idle());
    CHECK(log.empty()); // получатель заблокирован на пустой очереди
    kernel::start(producer(&q, 42), 0);
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{42})); // direct handoff, мимо буфера
    CHECK(q.size() == 0);
}

RTOS_TEST(queue_send_blocks_on_full) {
    Queue<int, 2> q;
    CHECK(q.try_send(1));
    CHECK(q.try_send(2));
    CHECK(!q.try_send(99)); // полна

    kernel::start(producer2(&q, 3), 0);
    CHECK(run_until_idle());
    CHECK(q.size() == 2); // отправитель заблокирован

    int v = 0;
    CHECK(q.try_receive(v) && v == 1); // будит отправителя, 3 ложится в буфер
    CHECK(run_until_idle());
    CHECK(q.size() == 2);
    CHECK(q.try_receive(v) && v == 2); // FIFO сохранён
    CHECK(q.try_receive(v) && v == 3);
}

RTOS_TEST(queue_isr_paths) {
    IntQueue q;
    std::vector<int> log;

    // ISR будит заблокированного получателя
    kernel::start(consumer(&q, &log), 0);
    CHECK(run_until_idle());
    CHECK(q.try_send_from_isr(7));
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{7}));

    // ISR не может писать в полную очередь без получателей
    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_send_from_isr(i));
    }
    CHECK(!q.try_send_from_isr(99));

    // ISR-чтение будит заблокированного отправителя
    kernel::start(producer(&q, 100), 0);
    CHECK(run_until_idle()); // producer заблокирован на полной очереди
    int v = -1;
    CHECK(q.try_receive_from_isr(v) && v == 0);
    CHECK(run_until_idle()); // producer дописал 100 и завершился
    CHECK(q.size() == 4);
    for (int expect : {1, 2, 3, 100}) {
        CHECK(q.try_receive(v) && v == expect);
    }
}

static Task recv_for(IntQueue* q, std::vector<int>* log, Tick timeout) {
    auto r = co_await q->receive_for(timeout);
    log->push_back(r ? *r : -1);
}

RTOS_TEST(queue_timeout_expires) {
    IntQueue q;
    std::vector<int> log;
    kernel::start(recv_for(&q, &log, 5), 0);
    CHECK(run_until_idle());
    advance_ticks(4);
    CHECK(log.empty());
    advance_ticks(1);
    CHECK((log == std::vector<int>{-1})); // таймаут

    // Поздний send попадает в буфер, а не «мёртвому» ждущему
    CHECK(q.try_send(9));
    CHECK(q.size() == 1);
    int v = 0;
    CHECK(q.try_receive(v) && v == 9);
}

RTOS_TEST(queue_event_beats_timeout) {
    IntQueue q;
    std::vector<int> log;
    kernel::start(recv_for(&q, &log, 5), 0);
    CHECK(run_until_idle());
    advance_ticks(4);
    CHECK(q.try_send_from_isr(11)); // событие на грани дедлайна
    CHECK(run_until_idle());
    CHECK((log == std::vector<int>{11}));
    advance_ticks(3); // просроченный дедлайн не будит второй раз
    CHECK((log == std::vector<int>{11}));
}

static Task send_for_task(IntQueue* q, std::vector<int>* log, int v,
                          Tick timeout) {
    bool ok = co_await q->send_for(v, timeout);
    log->push_back(ok ? v : -v);
}

RTOS_TEST(queue_send_timeout) {
    IntQueue q;
    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_send(i));
    }
    std::vector<int> log;
    kernel::start(send_for_task(&q, &log, 5, 3), 0);
    CHECK(run_until_idle());
    advance_ticks(3);
    CHECK((log == std::vector<int>{-5})); // не влезло за 3 тика
    int v = 0;
    while (q.try_receive(v)) {
    }
}
