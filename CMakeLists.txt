# SPDX-License-Identifier: MIT
# Copyright (c) 2026 X Ray

# =============================================================================
# coortos — кооперативный RTOS на stackless-корутинах C++26 (header-only).
# Самостоятельный проект: ядро (include/, src/), host-тесты (test/) и
# вспомогательный CMake-модуль генерации стартапа (cmake/).
#
# Подключение из проекта прошивки (каталог рядом либо любой другой путь):
#   set(COORTOS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../coortos CACHE PATH "")
#   set(RTOS_PORT CORTEX_M)         # RISCV32 | HOST; по умолчанию — авто
#   add_subdirectory(${COORTOS_DIR} ${CMAKE_BINARY_DIR}/coortos)
#   target_link_libraries(app PRIVATE coortos::coortos)
#
# Либо FetchContent_Declare(coortos GIT_REPOSITORY ... ) +
# FetchContent_MakeAvailable(coortos) — опции задаются так же, до него.
#
# Опции (задавать до add_subdirectory: set(...) либо -D при конфигурации):
#
#   RTOS_PORT = CORTEX_M | RISCV32 | HOST
#     Порт ядра. По умолчанию: при кросс-компиляции выводится из
#     CMAKE_SYSTEM_PROCESSOR (arm* -> CORTEX_M, riscv* -> RISCV32),
#     иначе HOST. CORTEX_M покрывает всё семейство (M0..M33, только
#     PRIMASK); RISCV32 — rv32 c Zicsr в machine mode (напр.
#     rv32imc_zicsr_zifencei / MIK32). CORTEX_M3 — устаревший синоним
#     CORTEX_M.
#
#   RTOS_CONTRACTS = ON | OFF (по умолчанию ON)
#     Контракты C++26 (P2900). Протокол ядра (критические секции у *_locked,
#     владение мьютексом, вызов только из задачи, инварианты очереди/пула)
#     описан pre()/contract_assert в заголовках.
#     OFF -> определяется RTOS_NO_CONTRACTS: RTOS_PRE/RTOS_POST — пустые
#     заглушки, RTOS_CONTRACT_ASSERT деградирует до всегда включённого
#     RTOS_ASSERT (трап). Вариант для компиляторов без поддержки контрактов.
#
#   RTOS_CONTRACT_SEMANTIC = ignore | observe | enforce | quick_enforce
#     Семантика проверки контрактов:
#       quick_enforce (по умолчанию для CORTEX_M3) — отладочная прошивка:
#                     проверка инлайнится в ~2 инструкции (cmp+bl),
#                     нарушение -> terminate -> abort.
#       ignore        — release: проверок в коде нет, ноль байт оверхеда;
#                     контракты остаются проверенной компилятором
#                     документацией.
#       observe       — печатает нарушение и продолжает. Требует printf =>
#                     практически только для host.
#       enforce       (по умолчанию для HOST) — печатает функцию, файл:строку
#                     и предикат, терминирует. С newlib НЕ ЛИНКУЕТСЯ
#                     (дефолтный обработчик тянет stdio); на MCU используйте
#                     quick_enforce либо определите свой
#                     ::handle_contract_violation.
#     -fcontracts добавляется и в link-опции: драйвер GCC подтягивает
#     libstdc++exp с обработчиком нарушений.
#
#   RTOS_PREEMPTIVE_WAKE = ON | OFF (по умолчанию OFF; порты CORTEX_M, HOST)
#     Вытесняющее пробуждение через PendSV: задача, разбуженная из ISR
#     (или из другой задачи) и СТРОГО более приоритетная, чем прерванная,
#     выполняется немедленно поверх неё, не дожидаясь её co_await, —
#     латентность реакции ограничена критическими секциями. Равные и
#     низшие приоритеты по-прежнему кооперативны (вытеснения посреди их
#     кода нет). Стек (MSP) рассчитывать на вложенность до
#     RTOS_NUM_PRIORITIES уровней. Прошивке нужен обработчик:
#       extern "C" void PendSV_Handler() { rtos::kernel::preempt_handler(); }
#     (examples/common/vectors.cpp уже содержит). На host эмулируется
#     на выходе из внешней критической секции — семантика тестируема.
#
#   RTOS_PROVIDE_ABORT = ON | OFF (по умолчанию ON, MCU-порты)
#     В каждый исполняемый файл-потребитель добавляется src/abort.cpp:
#     abort() = короткий трап (udf) с запрещёнными прерываниями вместо
#     raise/signal-механики newlib — меньше flash, смерть видна отладчику.
#
#   COORTOS_BUILD_TESTS = ON | OFF
#     Собирать host-тесты ядра (test/). По умолчанию ON, когда coortos —
#     верхний проект сборки и RTOS_PORT=HOST; как подпроект прошивки —
#     всегда OFF.
#
# Тюнинги самого ядра (число приоритетов, размер/число блоков пула фреймов)
# задаются макросами на целевом таргете — см. include/rtos/config.hpp:
#   target_compile_definitions(app PRIVATE RTOS_FRAME_BLOCK_COUNT=4 ...)
#
# Примеры вариантов:
#   отладка прошивки:  (по умолчанию)          quick_enforce + свой abort
#   release прошивки:  -DRTOS_CONTRACT_SEMANTIC=ignore
#   без контрактов:    -DRTOS_CONTRACTS=OFF
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(
  coortos
  VERSION 0.3.0
  DESCRIPTION "Кооперативный RTOS на stackless-корутинах C++26"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(COORTOS_TOP_LEVEL ON)
else()
  set(COORTOS_TOP_LEVEL OFF)
endif()

# Функция rtos_generate_cpp_startup() — генерация C++-стартапа из CMSIS
# startup .s. Экспортируется потребителю; применять по желанию (пример —
# опция RTOS_CPP_STARTUP в проекте прошивки STM32F103).
include(${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateCppStartup.cmake)

if(NOT DEFINED RTOS_PORT)
  if(CMAKE_CROSSCOMPILING)
    # CMAKE_SYSTEM_PROCESSOR тулчейн-файлы выставляют не всегда
    # (stm32-cmake — нет), поэтому смотрим и на компилятор.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Aa]rm|cortex)" OR CMAKE_CXX_COMPILER
                                                            MATCHES "arm-")
      set(RTOS_PORT CORTEX_M)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^riscv" OR CMAKE_CXX_COMPILER
                                                      MATCHES "riscv")
      set(RTOS_PORT RISCV32)
    else()
      message(
        FATAL_ERROR
          "Не удалось вывести RTOS_PORT (CMAKE_SYSTEM_PROCESSOR="
          "'${CMAKE_SYSTEM_PROCESSOR}', компилятор '${CMAKE_CXX_COMPILER}') "
          "— задайте set(RTOS_PORT ...) явно.")
    endif()
  else()
    set(RTOS_PORT HOST)
  endif()
endif()
if(RTOS_PORT STREQUAL "CORTEX_M3") # устаревший синоним
  set(RTOS_PORT CORTEX_M)
endif()
if(NOT RTOS_PORT MATCHES "^(CORTEX_M|RISCV32|HOST)$")
  message(FATAL_ERROR "RTOS_PORT=${RTOS_PORT}; "
                      "допустимо: CORTEX_M | RISCV32 | HOST")
endif()

option(RTOS_CONTRACTS "Компилировать контракты C++26 в ядре" ON)

if(NOT DEFINED RTOS_CONTRACT_SEMANTIC)
  if(RTOS_PORT STREQUAL "HOST")
    set(RTOS_CONTRACT_SEMANTIC enforce)
  else()
    set(RTOS_CONTRACT_SEMANTIC quick_enforce)
  endif()
endif()
if(NOT RTOS_CONTRACT_SEMANTIC MATCHES "^(ignore|observe|enforce|quick_enforce)$")
  message(FATAL_ERROR "RTOS_CONTRACT_SEMANTIC=${RTOS_CONTRACT_SEMANTIC}; "
                      "допустимо: ignore | observe | enforce | quick_enforce")
endif()

option(RTOS_PROVIDE_ABORT "Подменять abort() libc на трап (MCU-порты)" ON)
option(RTOS_PREEMPTIVE_WAKE "Вытесняющее пробуждение через PendSV" OFF)

add_library(coortos INTERFACE)
add_library(coortos::coortos ALIAS coortos)

target_include_directories(coortos
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(coortos INTERFACE RTOS_PORT_${RTOS_PORT})

if(RTOS_CONTRACTS)
  target_compile_options(
    coortos INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-fcontracts
                      -fcontract-evaluation-semantic=${RTOS_CONTRACT_SEMANTIC}>)
  target_link_options(coortos INTERFACE -fcontracts)
  if(RTOS_CONTRACT_SEMANTIC STREQUAL "enforce" AND NOT RTOS_PORT STREQUAL
                                                   "HOST")
    message(
      WARNING
        "RTOS_CONTRACT_SEMANTIC=enforce: дефолтный обработчик нарушений "
        "не линкуется с newlib-nano; нужен свой ::handle_contract_violation "
        "или quick_enforce.")
  endif()
else()
  target_compile_definitions(coortos INTERFACE RTOS_NO_CONTRACTS)
endif()

if(RTOS_PREEMPTIVE_WAKE)
  if(RTOS_PORT STREQUAL "RISCV32")
    message(FATAL_ERROR "RTOS_PREEMPTIVE_WAKE не поддержан на RISCV32 "
                        "(нужен software interrupt конкретной платформы)")
  endif()
  target_compile_definitions(coortos INTERFACE RTOS_PREEMPTIVE_WAKE)
endif()

if(RTOS_PROVIDE_ABORT AND NOT RTOS_PORT STREQUAL "HOST")
  # INTERFACE-исходник компилируется только в исполняемые файлы-потребители:
  # объект в составе executable перекрывает abort из архива libc, а в
  # статические библиотеки (board и т.п.) лишняя копия не попадает.
  target_sources(
    coortos
    INTERFACE
      $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${CMAKE_CURRENT_SOURCE_DIR}/src/abort.cpp>
  )
endif()

# --- Host-тесты ядра (test/) -------------------------------------------------
# Свои цели сборки и санитайзеры; как подпроект прошивки не собираются.
if(COORTOS_TOP_LEVEL AND RTOS_PORT STREQUAL "HOST")
  set(_coortos_tests_default ON)
else()
  set(_coortos_tests_default OFF)
endif()
option(COORTOS_BUILD_TESTS "Собирать host-тесты ядра" ${_coortos_tests_default})

if(COORTOS_BUILD_TESTS)
  if(NOT RTOS_PORT STREQUAL "HOST")
    message(FATAL_ERROR "COORTOS_BUILD_TESTS требует RTOS_PORT=HOST "
                        "(сейчас ${RTOS_PORT})")
  endif()
  enable_testing()
  add_subdirectory(test)
endif()
