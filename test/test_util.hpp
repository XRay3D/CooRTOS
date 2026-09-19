// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#pragma once

#include <cstdio>

#include <rtos/rtos.hpp>

inline int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("    CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                        #cond);                                                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (false)

struct TestCase {
    const char* name;
    void (*fn)();
    TestCase* next = nullptr;
    static inline TestCase* list = nullptr;

    TestCase(const char* n, void (*f)()) : name(n), fn(f) {
        TestCase** p = &list;
        while (*p) {
            p = &(*p)->next;
        }
        *p = this;
    }
};

#define RTOS_TEST(name)                                                        \
    static void test_##name();                                                 \
    static TestCase reg_##name{#name, &test_##name};                           \
    static void test_##name()

// Прогнать планировщик до опустошения ready-списков.
// false — не опустел за max_steps (зацикливание).
inline bool run_until_idle(int max_steps = 100000) {
    while (rtos::kernel::run_one()) {
        if (--max_steps == 0) {
            return false;
        }
    }
    return true;
}

// n тиков SysTick, после каждого — прогон разбуженных задач.
inline void advance_ticks(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        rtos::kernel::tick_isr();
        CHECK(run_until_idle());
    }
}
