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

# -isystem подавляет предупреждения из вендоренной библиотеки (third_party).
FLAGS="-std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread -Iinclude -isystem third_party"
CORE="src/core/Money.cpp src/core/Types.cpp src/core/Account.cpp src/core/Cashbox.cpp src/core/Operation.cpp"
CONFIG="src/config/ConfigLoader.cpp"
REPORTING="src/reporting/Logger.cpp"
ENGINE="src/engine/ServiceTimeProvider.cpp src/engine/ServiceStages.cpp src/engine/ClientFactory.cpp src/engine/SimulationRunner.cpp src/engine/AtmEngine.cpp"
SCENE="src/console/scene/GlyphSet.cpp src/console/scene/SceneCanvas.cpp src/console/scene/SceneSprites.cpp src/console/scene/SceneComposer.cpp src/console/scene/ScenePresenter.cpp src/console/scene/ActorAnim.cpp"
CONSOLE="src/console/CommandParser.cpp src/console/Terminal.cpp src/console/LiveRenderer.cpp src/console/Signals.cpp src/console/AdminConsole.cpp $SCENE"
TEST_CONSOLE="src/console/CommandParser.cpp src/console/Terminal.cpp src/console/LiveRenderer.cpp $SCENE"
TESTS="tests/test_main.cpp tests/test_money.cpp tests/test_account.cpp tests/test_cashbox.cpp tests/test_operation.cpp tests/test_config.cpp tests/test_service_time.cpp tests/test_service_stages.cpp tests/test_simulation.cpp tests/test_engine.cpp tests/test_parser.cpp tests/test_renderer.cpp tests/test_scene_canvas.cpp tests/test_scene_compose.cpp tests/test_scene_presenter.cpp tests/test_logger.cpp"

echo "== build unit tests =="
g++ $FLAGS -Itests $CORE $CONFIG $REPORTING $ENGINE $TEST_CONSOLE $TESTS -o build/atmsim_tests

echo "== build atm_sim =="
g++ $FLAGS src/main.cpp $CORE $CONFIG $REPORTING $ENGINE $CONSOLE -o build/atm_sim

echo "== run unit tests =="
./build/atmsim_tests
