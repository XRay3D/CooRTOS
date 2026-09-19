# SPDX-License-Identifier: MIT
# Copyright (c) 2026 X Ray

# rtos_generate_cpp_startup — генерация C++-стартапа из CMSIS startup .s.
#
# Парсит таблицу векторов (g_pfnVectors) и .equ-константы оригинального
# ассемблерного стартапа и генерирует эквивалентный C++-файл:
#   - таблица векторов в секции .isr_vector (константная, во flash);
#   - Reset_Handler: init .data/.bss, SystemInit (если есть), static-ctors, main;
#   - для обычных обработчиков — слабые алиасы на Default_Handler
#     (бесконечный цикл, дружелюбный к отладчику), как в CMSIS;
#   - для обработчиков из списка REQUIRE — ЖЁСТКИЕ ссылки: отсутствие
#     определения означает ошибку линковки, а не тихую заглушку. Это
#     защита от классических граблей weak-символов против архивов.
#
# Оригинальный .s после этого нужно исключить из сборки (см. корневой
# CMakeLists.txt: фильтрация INTERFACE_SOURCES CMSIS-таргета).
#
# rtos_generate_cpp_startup(
#     STARTUP <путь/к/startup_xxx.s>
#     OUTPUT  <путь/к/generated.cpp>
#     [REQUIRE <обработчик>...])

function(rtos_generate_cpp_startup)
  cmake_parse_arguments(GEN "" "STARTUP;OUTPUT" "REQUIRE" ${ARGN})
  if(NOT GEN_STARTUP OR NOT EXISTS "${GEN_STARTUP}")
    message(FATAL_ERROR "rtos_generate_cpp_startup: STARTUP-файл не найден: "
                        "'${GEN_STARTUP}'")
  endif()
  if(NOT GEN_OUTPUT)
    message(FATAL_ERROR "rtos_generate_cpp_startup: не задан OUTPUT")
  endif()

  file(READ "${GEN_STARTUP}" asm_text)

  # .equ-константы (BootRAM и т.п.)
  string(REGEX MATCHALL
         "\\.equ[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*,[ \t]*0[xX][0-9A-Fa-f]+"
         equs "${asm_text}")
  set(equ_names "")
  foreach(e IN LISTS equs)
    string(REGEX REPLACE
           "\\.equ[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*,[ \t]*(0[xX][0-9A-Fa-f]+)"
           "\\1;\\2" pair "${e}")
    list(GET pair 0 n)
    list(GET pair 1 v)
    list(APPEND equ_names "${n}")
    set(equ_${n} "${v}")
  endforeach()

  # Таблица векторов: вырезаем кусок от метки до .size
  # (regex не годится: в CMake «.» не матчит перевод строки)
  string(FIND "${asm_text}" "g_pfnVectors:" tbl_begin)
  if(tbl_begin EQUAL -1)
    message(FATAL_ERROR "rtos_generate_cpp_startup: в '${GEN_STARTUP}' "
                        "не найдена таблица g_pfnVectors")
  endif()
  string(SUBSTRING "${asm_text}" ${tbl_begin} -1 tbl)
  string(FIND "${tbl}" ".size" tbl_end)
  if(NOT tbl_end EQUAL -1)
    string(SUBSTRING "${tbl}" 0 ${tbl_end} tbl)
  endif()
  string(REGEX MATCHALL "\\.word[ \t]+[A-Za-z0-9_]+" words "${tbl}")
  set(entries "")
  foreach(w IN LISTS words)
    string(REGEX REPLACE "\\.word[ \t]+" "" sym "${w}")
    list(APPEND entries "${sym}")
  endforeach()

  list(LENGTH entries n_entries)
  if(n_entries LESS 3)
    message(FATAL_ERROR "rtos_generate_cpp_startup: таблица подозрительно "
                        "коротка (${n_entries} записей)")
  endif()
  list(GET entries 0 first)
  list(GET entries 1 second)
  if(NOT first STREQUAL "_estack" OR NOT second STREQUAL "Reset_Handler")
    message(
      FATAL_ERROR
        "rtos_generate_cpp_startup: неожиданное начало таблицы: "
        "'${first}', '${second}' (ожидались _estack, Reset_Handler)")
  endif()

  # Объявления и строки таблицы
  set(decls "")
  set(rows "    {.stack_top = _estack},\n    {.handler = Reset_Handler},\n")
  set(seen "")
  list(SUBLIST entries 2 -1 tail)
  foreach(sym IN LISTS tail)
    if(sym STREQUAL "0")
      string(APPEND rows "    {.handler = nullptr},\n")
    elseif(sym IN_LIST equ_names)
      string(APPEND rows "    {.raw = ${equ_${sym}}u}, // ${sym}\n")
    else()
      if(NOT sym IN_LIST seen)
        list(APPEND seen "${sym}")
        if(sym IN_LIST GEN_REQUIRE)
          string(APPEND decls
                 "extern \"C\" void ${sym}(); "
                 "// обязателен: нет определения — ошибка линковки\n")
        else()
          string(APPEND decls
                 "extern \"C\" void ${sym}() "
                 "__attribute__((weak, alias(\"Default_Handler\")));\n")
        endif()
      endif()
      string(APPEND rows "    {.handler = ${sym}},\n")
    endif()
  endforeach()

  foreach(r IN LISTS GEN_REQUIRE)
    if(NOT r IN_LIST seen)
      message(FATAL_ERROR "rtos_generate_cpp_startup: REQUIRE-обработчика "
                          "'${r}' нет в таблице векторов '${GEN_STARTUP}'")
    endif()
  endforeach()

  get_filename_component(src_name "${GEN_STARTUP}" NAME)
  set(content
"// Сгенерировано rtos_generate_cpp_startup из ${src_name} — не редактировать.
// ${n_entries} записей таблицы векторов.

#include <cstdint>

// Объявление и вызов ::main из стартапа — законная вольность
// (ISO запрещает, реальные рантаймы делают именно так)
#pragma GCC diagnostic ignored \"-Wpedantic\"

extern \"C\" {
extern std::uint32_t _estack[];
extern std::uint32_t _sidata[], _sdata[], _edata[], _sbss[], _ebss[];
void __libc_init_array();
int main();
void SystemInit() __attribute__((weak)); // system_*.c, если слинкован
}

// Необработанное прерывание: бесконечный цикл (как в CMSIS) — отладчик
// покажет активный вектор в IPSR; сторожевой таймер, если включён, сбросит.
extern \"C\" void Default_Handler() {
    for (;;) {
    }
}

${decls}
extern \"C\" [[noreturn, gnu::used]] void Reset_Handler() {
    const std::uint32_t* src = _sidata;
    for (std::uint32_t* dst = _sdata; dst != _edata; ++dst, ++src) {
        *dst = *src;
    }
    for (std::uint32_t* dst = _sbss; dst != _ebss; ++dst) {
        *dst = 0;
    }
    if (&SystemInit != nullptr) {
        SystemInit();
    }
    __libc_init_array();
    main();
    for (;;) {
    }
}

union VectorEntry {
    void (*handler)();
    std::uint32_t* stack_top;
    std::uint32_t raw;
};

extern \"C\" __attribute__((used, section(\".isr_vector\")))
const VectorEntry g_pfnVectors[] = {
${rows}};
")

  file(WRITE "${GEN_OUTPUT}.in" "${content}")
  configure_file("${GEN_OUTPUT}.in" "${GEN_OUTPUT}" COPYONLY)
  message(STATUS "coortos: C++-стартап сгенерирован из ${src_name} "
                 "(${n_entries} векторов) -> ${GEN_OUTPUT}")
endfunction()
