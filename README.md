# coortos — кооперативный RTOS на корутинах C++26

Ядро реального времени для микроконтроллеров, в котором задача — это
stackless-корутина C++, а не поток со своим стеком. Набор примитивов как у
FreeRTOS (задачи с приоритетами, очереди, семафоры, мьютекс, группы событий,
sleep/таймауты, пробуждение из прерываний), но без переключения контекста:
живое состояние задачи лежит в её фрейме, машинный стек — один на всю
систему.

Ядро header-only: заголовки в [include/rtos/](include/rtos/), единственный
`.cpp` ([src/abort.cpp](src/abort.cpp)) — необязательная подмена `abort()`.
Учёт ядра не аллоцирует (интрузивные списки живут прямо в фреймах корутин),
фреймы задач берутся из статического пула блоков либо из кучи.

**Порты:** **CORTEX_M** (всё семейство M0…M33, только PRIMASK), **RISCV32**
(rv32 + Zicsr, machine mode — MIK32 и т. п.), **HOST** (x86-заглушка для
юнит-тестов).

| | coortos | RTOS со стеком на задачу |
|---|---|---|
| Контекст задачи | фрейм корутины: только живые на `co_await` переменные (десятки байт) | стек, выделенный с запасом на худший случай |
| Переключение | `co_await` — регистры не сохраняются, это обычный возврат | сохранение регистров + смена SP |
| Вытеснение | только в точках `co_await`; опционально — приоритетное пробуждение поверх прерванной задачи (PendSV) | в любой точке |
| Блокировка из вызванной функции | нельзя: `co_await` — только в корутине (`Async<T>`) | можно откуда угодно |
| Переполнение стека задачи | стека задачи нет вовсе; отказ — при нехватке блока пула, в момент запуска. Общий стек нужен лишь на глубину вызовов и вложенность вытеснения | тихая порча памяти |

## Состав проекта

```
coortos/
├── CMakeLists.txt                  библиотека coortos::coortos + опции сборки
├── include/rtos/
│   ├── rtos.hpp                    зонтичный заголовок (включает всё)
│   ├── kernel.hpp                  планировщик, тик, вытеснение
│   ├── task.hpp                    Task, Async<T>, конверсия chrono → тики
│   ├── time.hpp                    sleep_for(), co_await 500ms
│   ├── queue.hpp                   Queue<T, N>
│   ├── semaphore.hpp               CountingSemaphore, BinarySemaphore
│   ├── mutex.hpp                   Mutex
│   ├── event_group.hpp             EventGroup (32 бита, семантика FreeRTOS)
│   ├── sync_wait.hpp               мост из не-корутинного кода в Async<T>
│   ├── pool.hpp                    пул фреймов корутин
│   ├── config.hpp                  тюнинги, макросы контрактов, хуки
│   ├── intrusive_list.hpp          списки ожидания без аллокаций
│   └── port/{cortex_m,riscv32,host}.hpp   критические секции, idle, трап
├── src/abort.cpp                   abort() = трап (опция RTOS_PROVIDE_ABORT)
├── cmake/GenerateCppStartup.cmake  генерация C++-стартапа из CMSIS startup .s
└── test/                           host-тесты ядра (ASan/UBSan, ctest)
```

## Требования

- **GCC ≥ 15** с контрактами C++26 (P2900, `-fcontracts`), режим `gnu++26`.
  Проверено на `arm-none-eabi-gcc 16.2` и x86 `GCC 16.2`.
- Компилятор без контрактов (GCC ≤ 14) — `-DRTOS_CONTRACTS=OFF`; тогда
  достаточно C++20 с корутинами (так собирается `../STMF031K6` на `gnu++20`),
  а проверки протокола ядра деградируют в безусловные трап-assert'ы (молча
  не исчезают).
- Исключения и RTTI не нужны (`-fno-exceptions -fno-rtti`): ядро не бросает,
  `unhandled_exception()` — трап.
- Куча не нужна: фреймы корутин по умолчанию из статического пула.

## Подключение

Проект подключается как CMake-подпроект; экспортируемый таргет —
`coortos::coortos` (INTERFACE: пути к заголовкам, `-DRTOS_PORT_*`, флаги
контрактов и, для MCU-портов, `src/abort.cpp`, добавляемый только в
исполняемые файлы). Опции задаются **до** подключения — обычным `set(...)`
либо `-D` при конфигурации.

Каталог рядом с проектом прошивки:

```cmake
set(COORTOS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../coortos
    CACHE PATH "Путь к проекту coortos")
set(RTOS_PORT CORTEX_M)          # RISCV32 | HOST; по умолчанию — авто
add_subdirectory(${COORTOS_DIR} ${CMAKE_BINARY_DIR}/coortos)

target_link_libraries(app PRIVATE coortos::coortos)
```

Либо через `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(coortos GIT_REPOSITORY <url> GIT_TAG <тег>)
set(RTOS_PORT CORTEX_M)
FetchContent_MakeAvailable(coortos)
```

### Что должно сделать приложение

1. **Тик.** Настроить системный таймер (SysTick, machine timer) и вызывать
   `rtos::kernel::tick_isr()` из его обработчика. Частота обязана совпадать
   с `RTOS_TICK_HZ` (по умолчанию 1000) — от неё считаются chrono-таймауты.

   ```cpp
   extern "C" void SysTick_Handler() { rtos::kernel::tick_isr(); }
   ```

2. **PendSV** — только при `RTOS_PREEMPTIVE_WAKE=ON` (порт CORTEX_M):

   ```cpp
   extern "C" void PendSV_Handler() { rtos::kernel::preempt_handler(); }
   ```

3. **Пул фреймов.** Прикинуть число одновременно живых корутин и размер
   самого большого фрейма — см. [«Память»](#память-фреймы-корутин).

4. Запустить задачи `kernel::start()` и отдать управление `kernel::run()`.

## Минимальный пример

```cpp
#include <rtos/rtos.hpp>

using namespace std::chrono_literals;

static rtos::Queue<char, 32> rx_q;

extern "C" void USART1_IRQHandler() {
    if (USART1->SR & USART_SR_RXNE)
        rx_q.try_send_from_isr(static_cast<char>(USART1->DR));
}

static rtos::Task echo() {
    for (;;) {
        char c = co_await rx_q.receive(); // блокируется, пока пусто
        board::uart1_write(c);
    }
}

static rtos::Task heartbeat() {
    for (;;) {
        board::led_toggle();
        co_await 500ms; // == co_await rtos::sleep_for(500) при 1 кГц
    }
}

int main() {
    board::clock_init_72mhz();
    board::systick_init(); // 1 кГц -> kernel::tick_isr()
    board::uart1_init(/*rx_irq=*/true);

    board::led_init();

    rtos::kernel::start(echo(), 0);      // 0 — высший приоритет
    rtos::kernel::start(heartbeat(), 1);
    rtos::kernel::run();                 // не возвращается
}
```

## Модель выполнения

- **Кооперативный планировщик со статическими приоритетами** (0 — высший,
  всего `RTOS_NUM_PRIORITIES`). Внутри приоритета — FIFO. Переключение
  происходит только в точках `co_await`; вытеснения посреди обычного кода
  нет, поэтому данные, к которым не прикасается ISR, не нуждаются в
  блокировках.
- **Прерывания.** ISR никогда не возобновляет корутину — только помечает
  задачу готовой (`*_from_isr`); возобновляет всегда цикл планировщика.
  Блокировка одна и глобальная — PRIMASK / `mstatus.MIE`; поэтому
  `*_from_isr` — это те же функции, что и обычные `try_*` (имена оставлены
  для читаемости и привычки к FreeRTOS).
- **Простой** — `WFI` под запрещёнными прерываниями (классическое закрытие
  гонки потерянного пробуждения), выход по тику или любому прерыванию.
- **Прямая передача (direct handoff).** Очередь, семафор и мьютекс отдают
  значение/владение ждущему напрямую, минуя буфер, в порядке FIFO: разбуженного
  нельзя «обокрасть» через `try_*` из другой задачи.
- **`RTOS_PREEMPTIVE_WAKE=ON`** — задача, разбуженная из ISR (или из другой
  задачи) и **строго** более приоритетная, чем прерванная, выполняется
  немедленно поверх неё через PendSV, не дожидаясь её `co_await`; латентность
  реакции ограничена длиной критических секций. Равные и низшие приоритеты
  остаются кооперативными. В точке `co_await` машинный стек корутины пуст,
  поэтому вложенный запуск на общем стеке корректен; стек (MSP) рассчитывать
  на вложенность до `RTOS_NUM_PRIORITIES` уровней. На HOST вытеснение
  эмулируется на выходе из внешней критической секции — семантика тестируема.
- **Реентерабельность.** `kernel::run_one()` можно звать из кода, вызванного
  самой задачей: вызывающая задача в этот момент Running и вне ready-списков,
  повторно не войдёт. Это позволяет `sync_wait()` — выполнить `Async<T>` до
  конца из главного цикла или из синхронного колбэка.

### Откуда что можно звать

| Контекст | Можно |
|---|---|
| Задача (`Task`) или `Async<T>` внутри неё | всё: `co_await`, `try_*`, `set`/`release`/`unlock`, `kernel::start` |
| ISR | `kernel::tick_isr()`, `try_send*`, `try_receive*`, `release*`, `set*`, `clear`; **никаких `co_await`** |
| Главный цикл до/вместо `kernel::run()` | `kernel::start`, `kernel::run_one`, `sync_wait`, все неблокирующие операции |

## Справочник API

Всё в `namespace rtos`, один заголовок `#include <rtos/rtos.hpp>`.
`Tick` = мс при `RTOS_TICK_HZ=1000` (32 бита, дедлайны wrap-safe: интервал до
~49.7 суток при 1 кГц). Методы с `co_await` — только из задач; `try_*` /
`*_from_isr` — неблокирующие, из ISR и задач.

**Время задаётся и тиками, и `std::chrono`**: `co_await 500ms` ==
`co_await sleep_for(500)`; все таймауты (`receive_for`, `send_for`,
`try_acquire_for`, `wait_any_for`, `wait_all_for`, `sleep_for`) принимают
chrono-длительности (`q.receive_for(1s)`), конверсия — с округлением вверх
(ждать не меньше запрошенного, 500us -> 1 тик).

### Задачи и планировщик — `task.hpp`, `kernel.hpp`, `time.hpp`, `sync_wait.hpp`

| API | Описание |
|---|---|
| `Task f(...)` | корутина-задача: обычная функция с `co_await`/`co_return`, обычно `for(;;)`. Detached: после `start` владеет планировщик, фрейм освобождается по завершении |
| `Async<T> g(...)` | вложенная корутина с результатом: `T v = co_await g(...)`; может блокироваться внутри (symmetric transfer) |
| `kernel::start(f(...), prio)` | поставить задачу в готовые (0 — высший приоритет); `false` — не удалось выделить фрейм |
| `kernel::run()` | главный цикл, не возвращается; WFI в простое |
| `kernel::run_one()` | выполнить одну готовую задачу (для тестов/интеграции); реентерабелен — задача может синхронно позвать код, который сам крутит `run_one()` (вложенные задачи исполняются поверх её стека, она сама повторно не входит) |
| `sync_wait(async, pump, prio)` | выполнить `Async<T>` до конца из не-корутинного контекста (главный цикл без `kernel::run()`, синхронный колбэк), крутя `pump()` в цикле; возвращает `T`. Из ISR звать нельзя |
| `kernel::tick_isr()` | вызывать из обработчика тика (SysTick, machine timer) |
| `kernel::now()` | текущий тик |
| `co_await kernel::yield()` | уступить: в хвост своей очереди приоритета |
| `co_await sleep_for(t)` | усыпить на `t` тиков или chrono-длительность |
| `co_await 500ms` (любая chrono-длительность) | то же, что `sleep_for` |
| `kernel::preempt_handler()` | тело `PendSV_Handler` (только `RTOS_PREEMPTIVE_WAKE`) |

### Очередь — `queue.hpp`: `Queue<T, N>`

| API | Описание |
|---|---|
| `co_await q.receive()` → `T` | ждать, пока пусто; значение от отправителя передаётся напрямую (direct handoff) |
| `co_await q.receive_for(t)` → `optional<T>` | то же с таймаутом |
| `co_await q.send(v)` | ждать, пока полно |
| `co_await q.send_for(v, t)` → `bool` | то же с таймаутом |
| `try_send(v)` / `try_send_from_isr(v)` → `bool` | не блокируясь; `false` — полна |
| `try_receive(out)` / `try_receive_from_isr(out)` → `bool` | не блокируясь; `false` — пуста |
| `size()`, `capacity()` | занято / ёмкость |

### Семафоры — `semaphore.hpp`

| API | Описание |
|---|---|
| `CountingSemaphore(initial, max)` / `BinarySemaphore(available=false)` | счётный / бинарный |
| `co_await s.acquire()` | ждать и захватить |
| `co_await s.try_acquire_for(t)` → `bool` | с таймаутом |
| `try_acquire()` → `bool` | не блокируясь |
| `release()` / `release_from_isr()` → `bool` | отдать; ждущему — напрямую (FIFO, не украсть через `try_acquire`); `false` — счётчик на максимуме |
| `count()` | текущий счётчик |

### Мьютекс — `mutex.hpp`: `Mutex` (только из задач, без наследования приоритетов)

| API | Описание |
|---|---|
| `co_await m.lock()` | захватить (не рекурсивный) |
| `try_lock()` → `bool` | не блокируясь |
| `unlock()` | отдать; следующему ждущему — прямая передача владения (FIFO) |
| `locked()` | занят ли |

### Группа событий — `event_group.hpp`: `EventGroup` (32 бита, семантика FreeRTOS)

| API | Описание |
|---|---|
| `co_await g.wait_any(bits, clear=true)` → `EventBits` | ждать любой из битов; результат — снимок в момент срабатывания |
| `co_await g.wait_all(bits, clear=true)` → `EventBits` | ждать все биты |
| `co_await g.wait_any_for/wait_all_for(bits, t, clear=true)` → `optional<EventBits>` | с таймаутом |
| `co_await g.sync(set_bits, wait_bits)` → `EventBits` | рандеву-барьер: выставить свои и дождаться всех; биты рандеву снимаются |
| `set(bits)` / `set_from_isr(bits)` | выставить; будит всех, чьё условие выполнилось, clear-on-exit — после прохода по всем |
| `clear(bits)` → прежние | снять биты |
| `get()` | текущие биты |

### Конфигурация и диагностика — `config.hpp`, `pool.hpp`

| API | Описание |
|---|---|
| `config::alloc_failed_hook` | указатель на обработчик нехватки/перероста фрейма (аргумент — реальный размер); `nullptr` — трап |
| `frame_pool.free_count()` | свободные блоки пула (диагностика) |
| `RTOS_ASSERT(x)` / `RTOS_CONTRACT_ASSERT(x)` / `RTOS_PRE(x)` | всегда-трап / контрактный assert / предусловие (см. [контракты](#контракты-c26)) |
| `port::trap()` | остановиться с запрещёнными прерываниями (`udf` / `ebreak`) |

## Память: фреймы корутин

Компилятор сам считает размер фрейма каждой корутины; ядро лишь выделяет под
него блок. По умолчанию — статический пул `RTOS_FRAME_BLOCK_COUNT` блоков по
`RTOS_FRAME_BLOCK_SIZE` байт (`RTOS_USE_MALLOC` переключает на кучу).

- **Сколько блоков.** Столько, сколько корутин живо одновременно: каждая
  запущенная `Task` плюс глубина вложенности `Async<T>` в каждой из них.
- **Какой размер блока.** По самому большому фактическому фрейму. Реальные
  размеры видно дизассемблером по вызовам `rtos::config::allocate` (аргумент —
  константа) или по срабатыванию `alloc_failed_hook`.
- **Отказ.** Фрейм больше блока или пул пуст → вызывается
  `config::alloc_failed_hook(requested)`; если хук не задан — трап. Если хук
  вернул управление, соответствующая `Task` невалидна (`operator bool` даёт
  `false`), а `kernel::start()` — `false`.

Пример бюджета из реального проекта на 4 КБ ОЗУ:
`RTOS_FRAME_BLOCK_SIZE=144 RTOS_FRAME_BLOCK_COUNT=7 RTOS_NUM_PRIORITIES=3`.

Сверх пула ядро не аллоцирует: узлы ожидания (`WaitNode`) лежат в awaiter'ах,
то есть внутри фреймов, а списки ядра интрузивные.

## Опции сборки

Опции CMake — до подключения проекта (подробности в [CMakeLists.txt](CMakeLists.txt)):

| Опция | По умолчанию | Смысл |
|---|---|---|
| `RTOS_PORT` | авто | `CORTEX_M` \| `RISCV32` \| `HOST`. Авто: при кросс-компиляции выводится из `CMAKE_SYSTEM_PROCESSOR`/имени компилятора, иначе `HOST`. `CORTEX_M3` — устаревший синоним `CORTEX_M` |
| `RTOS_CONTRACTS` | ON | OFF → макросы-заглушки: `RTOS_PRE` пуст, `RTOS_CONTRACT_ASSERT` → трап-assert; `-fcontracts` не нужен |
| `RTOS_CONTRACT_SEMANTIC` | `quick_enforce` (MCU), `enforce` (host) | `ignore` — release без оверхеда; `observe`; `enforce` на MCU требует своего `handle_contract_violation` |
| `RTOS_PROVIDE_ABORT` | ON (MCU) | `abort()` = короткий трап вместо raise/signal-механики newlib (~1 КБ flash) |
| `RTOS_PREEMPTIVE_WAKE` | OFF | вытесняющее пробуждение через PendSV (CORTEX_M, HOST); нужен `PendSV_Handler` → `kernel::preempt_handler()`; стек — на вложенность до `RTOS_NUM_PRIORITIES` |
| `COORTOS_BUILD_TESTS` | ON, если coortos — верхний проект и порт HOST | собирать host-тесты из `test/` |

Тюнинги ядра — макросы на своём таргете через `target_compile_definitions`:

| Макрос | По умолчанию | Смысл |
|---|---|---|
| `RTOS_NUM_PRIORITIES` | 4 | число приоритетов (0 — высший) |
| `RTOS_TICK_HZ` | 1000 | частота тика; должна совпадать с настройкой таймера — от неё считается конверсия chrono |
| `RTOS_FRAME_BLOCK_SIZE` | 256 | размер блока пула под фрейм корутины |
| `RTOS_FRAME_BLOCK_COUNT` | 8 | число блоков |
| `RTOS_USE_MALLOC` | выкл | фреймы из кучи вместо пула |
| `RTOS_NO_CONTRACTS` | выкл | принудительно отключить контракты (то же, что `RTOS_CONTRACTS=OFF`) |

## Контракты C++26

Протокол ядра (критические секции у `*_locked`, владение мьютексом, вызов
только из задачи, инварианты очереди и пула) описан `pre()`/`contract_assert`
(P2900, GCC ≥ 15). Семантика проверок выбирается сборкой:

| Сборка | Семантика | Цена |
|---|---|---|
| host-тесты | `enforce` | печатает функцию, файл:строку и предикат, терминирует |
| отладочная прошивка | `quick_enforce` | ~2 инструкции на проверку (`cmp`+`bl`), нарушение → `terminate` → `abort` → трап |
| release | `ignore` | ноль байт: контракты остаются проверенной компилятором документацией |

При `RTOS_CONTRACTS=OFF` проверки не исчезают молча: `RTOS_CONTRACT_ASSERT`
деградирует до безусловного `RTOS_ASSERT` (трап). `-fcontracts` добавляется и
в link-опции: драйвер GCC подтягивает `libstdc++exp` с обработчиком нарушений.
Дефолтный обработчик `enforce` тянет stdio и **не линкуется с newlib-nano** —
на MCU используйте `quick_enforce` либо определите свой
`::handle_contract_violation`.

## Порты

| Порт | Критическая секция | Простой | Вытеснение | Требуется от платформы |
|---|---|---|---|---|
| `CORTEX_M` | PRIMASK (`cpsid i`) | `WFI` | PendSV (SHPR3/ICSR) | тик → `tick_isr()`; при вытеснении — `PendSV_Handler`. Без CMSIS, только inline asm; годится для M0/M0+/M3/M4/M7/M23/M33 |
| `RISCV32` | `mstatus.MIE` (Zicsr) | `WFI` | не поддержано (нужен software interrupt конкретной платформы) | machine mode, тик от machine timer → `tick_isr()` |
| `HOST` | счётчик вложенности с проверкой баланса | `idle_hook` (без хука — диагностика дедлока) | эмуляция PendSV на выходе из внешней критической секции | — |

Свой порт — это `include/rtos/port/<имя>.hpp` с
`irq_save`/`irq_restore`/`irq_disable`/`irq_enable`, `in_critical()`,
RAII-`CriticalSection`, `idle()`, `[[noreturn]] trap()` (и `pend_preempt()`
/`preempt_init()`, если нужно вытеснение) плюс ветка в
[port.hpp](include/rtos/port.hpp) и в [CMakeLists.txt](CMakeLists.txt).

## Генерация C++-стартапа (необязательно)

[cmake/GenerateCppStartup.cmake](cmake/GenerateCppStartup.cmake) даёт функцию
`rtos_generate_cpp_startup()`: она парсит таблицу векторов и `.equ`-константы
оригинального ассемблерного стартапа CMSIS и генерирует эквивалентный C++
(таблица векторов в `.isr_vector`, `Reset_Handler` с инициализацией
`.data`/`.bss`, `SystemInit`, статическими конструкторами и `main`).

Обработчики из списка `REQUIRE` линкуются **жёсткими** ссылками: отсутствие
`SysTick_Handler` (или `PendSV_Handler` при вытеснении) становится ошибкой
линковки, а не тихой weak-заглушкой, которая молча остановит время.

```cmake
rtos_generate_cpp_startup(
  STARTUP ${CMSIS_F1_F103xB_STARTUP}
  OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/generated/startup_stm32f103xb.cpp
  REQUIRE SysTick_Handler PendSV_Handler)
```

Оригинальный `.s` после этого нужно исключить из сборки — как это сделано,
видно в корневом `CMakeLists.txt` проекта `STM32F103` (опция
`RTOS_CPP_STARTUP`).

## Host-тесты

Тесты гоняют ядро на x86: тик и «прерывания» дёргаются вручную, порт-слой —
host-заглушка, сборка с ASan/UBSan и контрактами `enforce`. После каждого
теста проверяется баланс критических секций и что тест не оставил висящих
задач.

```sh
cmake -S . -B build-host -DCMAKE_CXX_COMPILER=/opt/gcc-16/bin/g++
cmake --build build-host -j16
ctest --test-dir build-host --output-on-failure
```

| Бинарник | Что покрывает |
|---|---|
| `rtos_tests` | задачи и приоритеты, время и таймауты, очередь, семафоры, мьютекс, `Async`/`sync_wait`, группы событий, chrono-API |
| `rtos_preempt_tests` | вся та же матрица плюс вытесняющее пробуждение (`RTOS_PREEMPTIVE_WAKE`) |
| `rtos_pool_tests` | политика статического пула с `RTOS_FRAME_BLOCK_COUNT=2`: исчерпание и переиспользование блоков |

Каталог `test/` можно конфигурировать и как отдельный корень сборки
(`cmake -S test -B build-host ...`) — он сам подтянет ядро.

## Ограничения

- **Кооперативность.** Задача, не доходящая до `co_await`, держит процессор.
  `RTOS_PREEMPTIVE_WAKE` спасает только более приоритетных.
- **Блокироваться можно лишь в корутине.** Обычная функция не умеет
  `co_await`; чтобы подождать внутри вызова, она должна быть `Async<T>`.
  Обратный мост — `sync_wait()`.
- **Мьютекс без наследования приоритетов** — возможна инверсия приоритетов;
  держите критические секции короткими.
- **`Mutex` — только из задач**, из ISR его брать нельзя (контракт).
- **Фрейм задачи выделяется при запуске**: `kernel::start()` может вернуть
  `false`, это надо проверять.
- Тик 32-битный: максимальный интервал ожидания ~49.7 суток при 1 кГц.

## Лицензия

[MIT](LICENSE), © 2026 X Ray. 
