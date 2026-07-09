#!/bin/sh
# ============================================================================
#  asan_docker.sh — прогон всех тестов под Address + UB санитайзерами (§11).
#
#  Ловит порчу памяти, выходы за границы, утечки и неопределённое поведение.
#  ASan и TSan несовместимы, поэтому это отдельный прогон (гонки ищет tsan_docker.sh).
#
#  Запуск:
#    docker run --rm -v "$PWD":/work -w /work php:8.3-cli sh scripts/asan_docker.sh
# ============================================================================
set -e

mkdir -p build

FLAGS="-std=c++20 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -Iinclude -isystem third_party -Itests"
CORE="src/core/Money.cpp src/core/Types.cpp src/core/Account.cpp src/core/Cashbox.cpp src/core/Operation.cpp"
CONFIG="src/config/ConfigLoader.cpp"
REPORTING="src/reporting/Logger.cpp"
ENGINE="src/engine/ServiceTimeProvider.cpp src/engine/ServiceStages.cpp src/engine/ClientFactory.cpp src/engine/SimulationRunner.cpp src/engine/AtmEngine.cpp"
TEST_CONSOLE="src/console/CommandParser.cpp src/console/Terminal.cpp src/console/LiveRenderer.cpp"
TESTS="tests/test_main.cpp tests/test_money.cpp tests/test_account.cpp tests/test_cashbox.cpp tests/test_operation.cpp tests/test_config.cpp tests/test_service_time.cpp tests/test_service_stages.cpp tests/test_simulation.cpp tests/test_engine.cpp tests/test_parser.cpp tests/test_renderer.cpp tests/test_logger.cpp"

echo "== compile with -fsanitize=address,undefined =="
g++ $FLAGS $CORE $CONFIG $REPORTING $ENGINE $TEST_CONSOLE $TESTS -o build/atmsim_tests_asan

echo "== run under ASan + UBSan =="
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/atmsim_tests_asan
