# ============================================================================
#  ATM Queue Simulator — корневой CMake-скрипт.
#
#  Как устроена сборка (кратко):
#    * Требуем C++20 — он нужен для std::jthread / std::stop_token /
#      std::condition_variable_any (см. §6.2 ТЗ: «прерываемое ожидание»).
#    * Заводим ОДНУ интерфейсную цель `atmsim_flags` с флагами предупреждений и
#      потоками — и подключаем её ко всем таргетам, чтобы флаги не дублировать.
#    * Доменное ядро вынесено в библиотеку `atmsim_core` (M1). На неё опираются
#      и исполняемый файл `atm_sim`, и тесты `atmsim_tests`.
#
#  Локальная сборка на машине разработчика:
#      cmake -S . -B build
#      cmake --build build
#      ./build/atm_sim            (Linux)   |   build\Debug\atm_sim.exe (MSVC)
#      ctest --test-dir build     (запуск юнит-тестов)
# ============================================================================

cmake_minimum_required(VERSION 3.20)

project(atm_simulator
    VERSION 0.1.0
    DESCRIPTION "Консольный симулятор работы банкомата (одноканальная СМО)"
    LANGUAGES CXX)

# --- Стандарт языка: строго C++20, без компиляторных расширений ---------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Тип сборки по умолчанию: Debug ------------------------------------------
# Для одноконфигурационных генераторов (Make/Ninja) удобно иметь Debug по
# умолчанию — тогда работают отладочные символы и (позже) санитайзеры.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Тип сборки" FORCE)
endif()

# --- Опции проекта -----------------------------------------------------------
option(ATMSIM_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" OFF)
option(ATMSIM_BUILD_TESTS        "Собирать юнит-тесты"             ON)
option(ATMSIM_BUILD_TOOLS        "Собирать вспомогательные инструменты (tools/)" OFF)

# --- Общая интерфейсная цель с флагами ---------------------------------------
# INTERFACE-библиотека не компилируется сама, а «раздаёт» свои свойства тем,
# кто к ней подключается. Здесь это: стандарт C++20, предупреждения, потоки.
add_library(atmsim_flags INTERFACE)
target_compile_features(atmsim_flags INTERFACE cxx_std_20)

# Потоки нужны всему проекту (std::jthread и пр.). На Linux это тянет -pthread.
find_package(Threads REQUIRED)
target_link_libraries(atmsim_flags INTERFACE Threads::Threads)

if(MSVC)
    target_compile_options(atmsim_flags INTERFACE /W4 /permissive-)
    if(ATMSIM_WARNINGS_AS_ERRORS)
        target_compile_options(atmsim_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(atmsim_flags INTERFACE -Wall -Wextra -Wpedantic)
    if(ATMSIM_WARNINGS_AS_ERRORS)
        target_compile_options(atmsim_flags INTERFACE -Werror)
    endif()
endif()

# --- Доменное ядро (M1): деньги, счёт, касса, операция ------------------------
# Библиотека без потоков и без внешних зависимостей — её легко покрывать тестами.
add_library(atmsim_core
    src/core/Money.cpp
    src/core/Types.cpp
    src/core/Account.cpp
    src/core/Cashbox.cpp
    src/core/Operation.cpp)
# PUBLIC include — все, кто линкует ядро, автоматически видят заголовки atmsim/.
target_include_directories(atmsim_core PUBLIC include)
target_link_libraries(atmsim_core PUBLIC atmsim_flags)

# --- Вендоренная библиотека nlohmann/json (single-header) ---------------------
# Подключаем как INTERFACE-цель c SYSTEM-путём: SYSTEM подавляет предупреждения
# из чужого кода, чтобы они не смешивались с нашими (-Wall -Wextra -Wpedantic).
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE third_party)

# --- Конфигурация (M2): загрузка и валидация JSON -----------------------------
add_library(atmsim_config
    src/config/ConfigLoader.cpp)
target_link_libraries(atmsim_config PUBLIC atmsim_core PRIVATE nlohmann_json)

# --- Отчётность (M7): технический лог в файл ---------------------------------
add_library(atmsim_reporting
    src/reporting/Logger.cpp)
target_include_directories(atmsim_reporting PUBLIC include)
target_link_libraries(atmsim_reporting PUBLIC atmsim_flags)

# --- Движок симуляции (M2 однопоточный + M3 многопоточный AtmEngine) ----------
add_library(atmsim_engine
    src/engine/ServiceTimeProvider.cpp
    src/engine/ServiceStages.cpp
    src/engine/ClientFactory.cpp
    src/engine/SimulationRunner.cpp
    src/engine/AtmEngine.cpp)
target_link_libraries(atmsim_engine PUBLIC atmsim_config atmsim_reporting)

# --- Консоль администратора (M3/M4 + M6 дашборд + M7 сигналы + сцена) ---------
# Подкаталог scene/ — анимированная сцена дашборда (feature/scene): клеточная
# канва, спрайты, презентационный слой актёров. Живёт в console-слое, потому
# что это чистая презентация поверх снимков движка.
add_library(atmsim_console
    src/console/CommandParser.cpp
    src/console/Terminal.cpp
    src/console/FrameDiffer.cpp
    src/console/LiveRenderer.cpp
    src/console/Signals.cpp
    src/console/AdminConsole.cpp
    src/console/scene/GlyphSet.cpp
    src/console/scene/SceneCanvas.cpp
    src/console/scene/SceneSprites.cpp
    src/console/scene/SceneComposer.cpp
    src/console/scene/ScenePresenter.cpp
    src/console/scene/ActorAnim.cpp)
target_link_libraries(atmsim_console PUBLIC atmsim_engine)
if(WIN32)
    # timeBeginPeriod/timeEndPeriod (TimerResolutionGuard в Terminal.cpp).
    # Для MSVC зависимость продублирована pragma comment(lib) в исходнике,
    # но MinGW/clang прагму игнорируют — линкуем явно.
    target_link_libraries(atmsim_console PUBLIC winmm)
endif()

# --- Исполняемый файл --------------------------------------------------------
add_executable(atm_sim
    src/main.cpp)
target_link_libraries(atm_sim PRIVATE atmsim_console)

# --- Вспомогательные инструменты (не собираются по умолчанию) -----------------
# term_bench — измеритель «бюджета кадра» терминала для фичи «сцена» (этап 0):
# самодостаточный бинарь без зависимостей от библиотек проекта. Методика и
# снятые цифры — в docs/scene_frame_budget.md.
if(ATMSIM_BUILD_TOOLS)
    add_executable(term_bench tools/term_bench.cpp)
    target_link_libraries(term_bench PRIVATE atmsim_flags)
    if(WIN32)
        # timeBeginPeriod/timeEndPeriod (замер режима hires) живут в winmm.
        target_link_libraries(term_bench PRIVATE winmm)
    endif()
endif()

# --- Тесты -------------------------------------------------------------------
# Пока используется минимальный собственный харнес (tests/simple_test.hpp) —
# см. примечание в README. Когда появится сеть, заменим на GoogleTest.
if(ATMSIM_BUILD_TESTS)
    enable_testing()
    add_executable(atmsim_tests
        tests/test_main.cpp
        tests/test_money.cpp
        tests/test_account.cpp
        tests/test_cashbox.cpp
        tests/test_operation.cpp
        tests/test_config.cpp
        tests/test_service_time.cpp
        tests/test_service_stages.cpp
        tests/test_simulation.cpp
        tests/test_engine.cpp
        tests/test_parser.cpp
        tests/test_renderer.cpp
        tests/test_scene_canvas.cpp
        tests/test_scene_compose.cpp
        tests/test_scene_presenter.cpp
        tests/test_frame_differ.cpp
        tests/test_logger.cpp)
    target_include_directories(atmsim_tests PRIVATE tests)
    target_link_libraries(atmsim_tests PRIVATE atmsim_console)
    add_test(NAME unit COMMAND atmsim_tests)
endif()
