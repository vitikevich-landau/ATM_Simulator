#!/bin/sh
# ============================================================================
#  tsan_docker.sh — прогон конкурентных тестов под ThreadSanitizer (§11, §14).
#
#  Ищет гонки данных в многопоточном ядре (AtmEngine). Собирает ТОЛЬКО то, что
#  нужно для конкурентных тестов (core + ServiceTimeProvider + AtmEngine +
#  test_engine), поэтому под TSan запускаются именно они.
#
#  Запуск:
#    docker run --rm -v "$PWD":/work -w /work php:8.3-cli sh scripts/tsan_docker.sh
# ============================================================================
set -e

mkdir -p build

CORE="src/core/Money.cpp src/core/Types.cpp src/core/Account.cpp src/core/Cashbox.cpp src/core/Operation.cpp"
FLAGS="-std=c++20 -g -O1 -fsanitize=thread -pthread -Iinclude -Itests"

echo "== compile with -fsanitize=thread =="
g++ $FLAGS $CORE src/reporting/Logger.cpp src/engine/ServiceTimeProvider.cpp src/engine/ServiceStages.cpp src/engine/AtmEngine.cpp \
    src/console/Terminal.cpp src/console/LiveRenderer.cpp \
    src/console/scene/GlyphSet.cpp src/console/scene/SceneCanvas.cpp \
    tests/test_main.cpp tests/test_engine.cpp tests/test_renderer.cpp tests/test_service_stages.cpp -o build/atmsim_tests_tsan

# ThreadSanitizer в контейнере спотыкается об ASLR ("unexpected memory mapping").
# setarch -R отключает рандомизацию адресов только для этого процесса — без
# --privileged и без изменения настроек хоста.
echo "== run under ThreadSanitizer (ASLR off via setarch) =="
setarch "$(uname -m)" -R ./build/atmsim_tests_tsan
