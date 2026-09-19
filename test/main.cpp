// SPDX-License-Identifier: MIT
// Copyright (c) 2026 X Ray

#include "test_util.hpp"

int main() {
    int ran = 0;
    for (TestCase* t = TestCase::list; t; t = t->next) {
        std::printf("[ RUN  ] %s\n", t->name);
        int before = g_failures;
        t->fn();
        CHECK(rtos::port::critical_depth == 0); // баланс критических секций
        CHECK(run_until_idle());                // тест не оставил хвостов
        std::printf("%s %s\n", g_failures == before ? "[  OK  ]" : "[ FAIL ]",
                    t->name);
        ++ran;
    }
    std::printf("%d tests, %d failures\n", ran, g_failures);
    return g_failures ? 1 : 0;
}
