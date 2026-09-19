// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <coroutine>
#include <cstdint>

#include "config.hpp"
#include "intrusive_list.hpp"
#include "port.hpp"
#include "task.hpp"

namespace rtos {

// true, если дедлайн a наступает не позже b (wrap-safe для uint32).
inline bool deadline_le(Tick a, Tick b) {
    return static_cast<std::int32_t>(a - b) <= 0;
}

// Узел ожидания. Живёт внутри awaiter'а, т.е. во фрейме корутины, поэтому
// учёт ожидания не требует аллокаций. Может состоять одновременно в
// wait-списке примитива и в delay-списке ядра (таймаут); пробуждение любым
// событием снимает узел с обоих списков в одной критической секции.
struct WaitNode {
    ListNode wait_link{this};
    ListNode delay_link{this};
    TaskPromise* task = nullptr;
    Tick deadline = 0;
    bool timed_out = false;
    void* payload = nullptr; // слот значения для direct handoff (Queue)
};

namespace kernel {

inline List ready_lists[RTOS_NUM_PRIORITIES]; // [0] — высший приоритет
inline List delay_list;                       // отсортирован по дедлайну
inline TaskPromise* current = nullptr;
inline volatile Tick tick_count = 0;

// Приоритет выполняющейся сейчас задачи (NoRunningTask — планировщик
// простаивает). Один байт: читается/пишется атомарно. В окне между
// возвратом из resume() и сбросом значение устаревшее — это безопасно:
// порог вытеснения лишь консервативнее (обработчик никогда не
// разыменовывает current — после завершения задачи он висячий).
inline constexpr std::uint8_t NoRunningTask = 0xFF;
inline volatile std::uint8_t running_prio = NoRunningTask;

inline Tick now() { return tick_count; }

// Все *_locked-функции вызываются только внутри критической секции —
// это описано контрактами RTOS_PRE(port::in_critical()).

inline void make_ready_locked(TaskPromise* p)
    RTOS_PRE(port::in_critical()) RTOS_PRE(p != nullptr) {
    if (p->state == TaskPromise::State::Ready) {
        return;
    }
    p->state = TaskPromise::State::Ready;
    ready_lists[p->priority].push_back(p->ready_link);
#ifdef RTOS_PREEMPTIVE_WAKE
    // Разбудили задачу приоритетнее выполняющейся — вытеснить (PendSV).
    // В простое (NoRunningTask) не пендим: главный цикл и так проснётся,
    // а kernel::start() до run() не должен исполнять задачи.
    if (running_prio != NoRunningTask && p->priority < running_prio) {
        port::pend_preempt();
    }
#endif
}

inline void delay_insert_locked(WaitNode& w, Tick deadline)
    RTOS_PRE(port::in_critical()) RTOS_PRE(!w.delay_link.linked()) {
    w.deadline = deadline;
    w.timed_out = false;
    ListNode* pos = delay_list.head.next;
    while (pos != &delay_list.head) {
        WaitNode* other = static_cast<WaitNode*>(pos->owner);
        if (!deadline_le(other->deadline, deadline)) {
            break;
        }
        pos = pos->next;
    }
    w.delay_link.insert_before(*pos);
}

// Заблокировать текущую задачу: запомнить точку возобновления, поставить
// в wait-список примитива (если есть) и/или в delay-список (если есть таймаут).
inline void block_current_locked(WaitNode& w, List* wait_list,
                                 std::coroutine_handle<> resume_point,
                                 bool has_deadline = false, Tick deadline = 0)
    RTOS_PRE(port::in_critical())
    RTOS_PRE(current != nullptr) // блокироваться можно только из задачи
{
    TaskPromise* t = current;
    RTOS_ASSERT(t != nullptr); // остаётся и при семантике ignore
    w.task = t;
    w.timed_out = false;
    t->resume_point = resume_point;
    t->state = TaskPromise::State::Blocked;
    if (wait_list) {
        wait_list->push_back(w.wait_link);
    }
    if (has_deadline) {
        delay_insert_locked(w, deadline);
    }
}

// Разбудить ждущего: снять с обоих списков и поставить в ready.
// wait_link к этому моменту может быть уже снят (pop_front) — unlink идемпотентен.
inline void wake_locked(WaitNode* w, bool timed_out = false)
    RTOS_PRE(port::in_critical()) RTOS_PRE(w != nullptr && w->task != nullptr) {
    w->wait_link.unlink();
    w->delay_link.unlink();
    w->timed_out = timed_out;
    make_ready_locked(w->task);
}

// Запустить задачу с приоритетом (0 — высший). false — задача невалидна
// (аллокация фрейма не удалась при возвращающем alloc_failed_hook).
inline bool start(Task&& t, std::uint8_t priority) {
    auto h = t.release();
    if (!h) {
        return false;
    }
    TaskPromise& p = h.promise();
    p.priority =
        priority < RTOS_NUM_PRIORITIES ? priority : RTOS_NUM_PRIORITIES - 1;
    p.resume_point = h;
    port::CriticalSection cs;
    make_ready_locked(&p);
    return true;
}

// Тик времени. Вызывается из SysTick_Handler (или вручную в тестах).
inline void tick_isr() {
    port::CriticalSection cs;
    tick_count = tick_count + 1;
    while (ListNode* n = delay_list.front()) {
        WaitNode* w = static_cast<WaitNode*>(n->owner);
        if (!deadline_le(w->deadline, tick_count)) {
            break; // список отсортирован — дальше только будущие дедлайны
        }
        wake_locked(w, /*timed_out=*/true);
    }
}

// Возобновить одну самую приоритетную готовую задачу.
// false — готовых нет.
//
// Реентерабелен: задача может синхронно позвать код, который сам крутит
// run_one() (например, главный контекст приложения, качающий планировщик
// внутри своего ожидания). Вызывающая задача в этот момент Running и вне
// ready-списков, поэтому повторно не войдёт; вложенные задачи исполняются
// поверх её машинного стека и возвращаются (co_await или завершение) до её
// продолжения. current/running_prio на выходе восстанавливаются, так что
// следующий co_await вызывающей задачи блокирует именно её.
inline bool run_one() {
    TaskPromise* p = nullptr;
    TaskPromise* const saved_current = current;
    const std::uint8_t saved_prio = running_prio;
    {
        port::CriticalSection cs;
        for (auto& l : ready_lists) {
            if (ListNode* n = l.pop_front()) {
                p = static_cast<TaskPromise*>(n->owner);
                break;
            }
        }
        if (p) {
            p->state = TaskPromise::State::Running;
            current = p;
            running_prio = p->priority;
        }
    }
    if (!p) {
        return false;
    }
    p->resume_point.resume(); // после resume() фрейм мог быть уничтожен
    current = saved_current;
    running_prio = saved_prio;
    return true;
}

#ifdef RTOS_PREEMPTIVE_WAKE
// Вытесняющее пробуждение: тело обработчика PendSV (на host — его
// эмуляции). Выполняет поверх прерванной задачи все готовые задачи
// СТРОГО более высокого приоритета — каждую до её блокировки; прерванная
// продолжится после возврата. Сам себя обработчик не вытесняет (PendSV —
// низший приоритет прерываний): задача, разбуженная из вложенной, будет
// подхвачена повторным проходом цикла. В точке co_await машинный стек
// корутины пуст, поэтому вложенный запуск на общем стеке корректен;
// стек нужно рассчитывать на глубину вложенности до RTOS_NUM_PRIORITIES.
inline void preempt_handler() {
    const std::uint8_t entry_prio = running_prio;
    TaskPromise* const saved_current = current; // только сохранить/вернуть
    for (;;) {
        TaskPromise* p = nullptr;
        {
            port::CriticalSection cs;
            const std::uint8_t limit = entry_prio < RTOS_NUM_PRIORITIES
                                           ? entry_prio
                                           : RTOS_NUM_PRIORITIES;
            for (std::uint8_t pr = 0; pr < limit; ++pr) {
                if (ListNode* n = ready_lists[pr].pop_front()) {
                    p = static_cast<TaskPromise*>(n->owner);
                    break;
                }
            }
            if (p) {
                p->state = TaskPromise::State::Running;
                current = p;
                running_prio = p->priority;
            }
        }
        if (!p) {
            break;
        }
        p->resume_point.resume(); // фрейм мог быть уничтожен
    }
    current = saved_current;
    running_prio = entry_prio;
}

#if defined(RTOS_PORT_HOST)
// Host-эмуляция аппаратного поведения PendSV: не вытесняет сам себя,
// повторный pend во время обработки даёт ещё один проход (tail-chaining).
inline bool preempt_active = false;
inline void preempt_dispatch() {
    if (preempt_active) {
        return; // pending остаётся взведён — добёрет внешний цикл
    }
    preempt_active = true;
    while (port::preempt_pending) {
        port::preempt_pending = false;
        preempt_handler();
    }
    preempt_active = false;
}
namespace detail {
[[maybe_unused]] inline const bool preempt_hook_installed =
    (port::preempt_dispatch = &preempt_dispatch, true);
} // namespace detail
#endif // RTOS_PORT_HOST
#endif // RTOS_PREEMPTIVE_WAKE

// Главный цикл планировщика. Не возвращается.
[[noreturn]] inline void run() {
#ifdef RTOS_PREEMPTIVE_WAKE
    port::preempt_init();
#endif
    for (;;) {
        // Классическое закрытие гонки потерянного пробуждения:
        // проверка ready и WFI — под запрещёнными прерываниями.
        port::irq_disable();
        bool any_ready = false;
        for (auto& l : ready_lists) {
            if (!l.empty()) {
                any_ready = true;
                break;
            }
        }
        if (any_ready) {
            port::irq_enable();
            run_one();
        } else {
            port::idle();
            port::irq_enable(); // здесь выполнится отложенный обработчик
        }
    }
}

// Уступить процессор: задача снова в хвост своей ready-очереди.
struct YieldAwaiter {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        port::CriticalSection cs;
        current->resume_point = h;
        make_ready_locked(current); // state Running -> Ready, в хвост очереди
    }
    void await_resume() noexcept {}
};

inline YieldAwaiter yield() { return {}; }

} // namespace kernel

} // namespace rtos
