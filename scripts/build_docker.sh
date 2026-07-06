#!/bin/sh
# ============================================================================
#  build_docker.sh — офлайн-сборка и прогон юнит-тестов напрямую через g++.
#
#  Используется, когда на машине нет CMake/компилятора: сборка идёт в Linux-
#  контейнере с GCC. Запуск из корня репозитория:
#
#      docker run --rm -v "$PWD":/work -w /work php:8.3-cli sh scripts/build_docker.sh
#
#  Штатный способ сборки на машине с тулчейном — CMake (см. README.md);
#  этот скрипт нужен только для проверки в контейнере без CMake.
# ============================================================================
set -e

mkdir -p build

FLAGS="-std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -Iinclude"
CORE="src/core/Money.cpp src/core/Types.cpp src/core/Account.cpp src/core/Cashbox.cpp src/core/Operation.cpp"
TESTS="tests/test_main.cpp tests/test_money.cpp tests/test_account.cpp tests/test_cashbox.cpp tests/test_operation.cpp"

echo "== build unit tests =="
g++ $FLAGS -Itests $CORE $TESTS -o build/atmsim_tests

echo "== build atm_sim =="
g++ $FLAGS src/main.cpp $CORE -o build/atm_sim

echo "== run unit tests =="
./build/atmsim_tests
