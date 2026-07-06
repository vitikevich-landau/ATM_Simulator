# ============================================================================
#  ATM Queue Simulator — корневой CMake-скрипт.
#
#  Как устроена сборка (кратко):
#    * Требуем C++20 — он нужен для std::jthread / std::stop_token /
#      std::condition_variable_any (см. §6.2 ТЗ: «прерываемое ожидание»).
#    * Заводим ОДНУ интерфейсную цель `atmsim_flags` с флагами предупреждений и
#      потоками — и подключаем её ко всем таргетам, чтобы флаги не дублировать.
#    * Пока в проекте только исполняемый файл `atm_sim` с точкой входа.
#      По мере роста (M1+) сюда добавятся библиотеки core/engine/console и тесты.
#
#  Локальная сборка на машине разработчика:
#      cmake -S . -B build
#      cmake --build build
#      ./build/atm_sim            (Linux)   |   build\Debug\atm_sim.exe (MSVC)
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

# --- Исполняемый файл --------------------------------------------------------
# Пока это только точка входа. Доменное ядро приедет в M1 отдельной библиотекой.
add_executable(atm_sim
    src/main.cpp)

target_include_directories(atm_sim PRIVATE include)
target_link_libraries(atm_sim PRIVATE atmsim_flags)
