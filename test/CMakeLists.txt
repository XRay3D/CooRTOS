# SPDX-License-Identifier: MIT
# Copyright (c) 2026 X Ray

# Host-тесты ядра coortos (x86, GCC 16 из /opt/gcc-16 — компилятор с
# поддержкой контрактов C++26). Собираются двумя способами:
#
#   1) вместе с проектом coortos (по умолчанию, когда он верхний и
#      порт HOST):
#        cmake -S . -B build-host -DCMAKE_CXX_COMPILER=/opt/gcc-16/bin/g++
#        cmake --build build-host -j16 && ctest --test-dir build-host
#
#   2) отдельным проектом — этот каталог как корень (НЕ через
#      ARM-toolchain прошивки):
#        cmake -S test -B build-host -DCMAKE_CXX_COMPILER=/opt/gcc-16/bin/g++
#
# Ядро подключается библиотекой coortos::coortos; без кросс-компиляции
# порт по умолчанию HOST, семантика контрактов — enforce: при нарушении
# печатается функция, файл:строка и предикат, процесс терминируется,
# тест падает.

cmake_minimum_required(VERSION 3.16)

if(NOT TARGET coortos::coortos) # вариант 2: этот каталог — корень сборки
  project(coortos_host_tests CXX)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/..
                   ${CMAKE_CURRENT_BINARY_DIR}/coortos)
  enable_testing()
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TEST_COMPILE_FLAGS -O1 -g -Wall -Wextra -Werror
                       -fsanitize=address,undefined)
set(TEST_LINK_FLAGS -fsanitize=address,undefined -Wl,-rpath,/opt/gcc-16/lib64
                    -Wl,-rpath,/opt/gcc-16/lib)

add_executable(
  rtos_tests
  main.cpp
  test_task.cpp
  test_time.cpp
  test_queue.cpp
  test_semaphore.cpp
  test_mutex.cpp
  test_async.cpp
  test_event_group.cpp
  test_chrono.cpp)
target_link_libraries(rtos_tests PRIVATE coortos::coortos)
target_compile_definitions(rtos_tests PRIVATE RTOS_USE_MALLOC)
target_compile_options(rtos_tests PRIVATE ${TEST_COMPILE_FLAGS})
target_link_options(rtos_tests PRIVATE ${TEST_LINK_FLAGS})

# Отдельный бинарник: вся матрица тестов + тесты вытесняющего пробуждения
# под RTOS_PREEMPTIVE_WAKE (host-эмуляция PendSV). Старые тесты обязаны
# проходить и в этом режиме: вне работающей задачи pend не взводится.
add_executable(
  rtos_preempt_tests
  main.cpp
  test_task.cpp
  test_time.cpp
  test_queue.cpp
  test_semaphore.cpp
  test_mutex.cpp
  test_async.cpp
  test_event_group.cpp
  test_chrono.cpp
  test_preempt.cpp)
target_link_libraries(rtos_preempt_tests PRIVATE coortos::coortos)
target_compile_definitions(rtos_preempt_tests PRIVATE RTOS_USE_MALLOC
                                                      RTOS_PREEMPTIVE_WAKE)
target_compile_options(rtos_preempt_tests PRIVATE ${TEST_COMPILE_FLAGS})
target_link_options(rtos_preempt_tests PRIVATE ${TEST_LINK_FLAGS})

# Отдельный бинарник: политика "статический пул" с крошечным пулом,
# чтобы проверить исчерпание и переиспользование блоков.
add_executable(rtos_pool_tests main.cpp test_pool.cpp)
target_link_libraries(rtos_pool_tests PRIVATE coortos::coortos)
target_compile_definitions(rtos_pool_tests PRIVATE RTOS_FRAME_BLOCK_COUNT=2)
target_compile_options(rtos_pool_tests PRIVATE ${TEST_COMPILE_FLAGS})
target_link_options(rtos_pool_tests PRIVATE ${TEST_LINK_FLAGS})

foreach(t rtos_tests rtos_preempt_tests rtos_pool_tests)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
