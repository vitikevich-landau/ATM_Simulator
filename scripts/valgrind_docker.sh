#!/bin/sh
# ============================================================================
#  valgrind_docker.sh — прогон под Valgrind memcheck (независимая от ASan/UBSan
#  проверка памяти: утечки, чтение неинициализированного, выход за границы).
#
#  Valgrind гоняет УЖЕ СОБРАННЫЙ бинарь на виртуальном CPU и проверяет КАЖДЫЙ
#  доступ к памяти во время исполнения — движок иной, чем у ASan (тот
#  инструментирует при компиляции), поэтому это честная перекрёстная проверка.
#  С санитайзерами НЕсовместим, поэтому собираем отдельно: -g -O1, БЕЗ
#  -fsanitize.
#
#  ВАЖНО про тайминги: под Valgrind программа идёт ~20–30x медленнее, а
#  планировщик потоков сильно перетасован. Часть конкурентных тестов завязана
#  на «нормальные» тайминги и МОЖЕТ ложно упасть по логике (не по памяти) —
#  это не утечка. Поэтому вердикт Valgrind по ПАМЯТИ (exit 99 при ошибках)
#  отделён от кода самой программы: скрипт валится только на реальных проблемах
#  памяти, а логический провал ребёнка лишь отмечается.
#
#  В образе php:8.3-cli Valgrind нет — доставляем apt-ом (нужна сеть).
#
#  Запуск:
#    docker run --rm -v "$PWD":/work -w /work php:8.3-cli sh scripts/valgrind_docker.sh
# ============================================================================
set -e

if ! command -v valgrind >/dev/null 2>&1; then
    echo "== установка valgrind (нужна сеть) =="
    apt-get update -qq && apt-get install -y -qq valgrind >/dev/null
fi
valgrind --version

mkdir -p build

# -g для имён/строк в отчёте, -O1 (не -O0: быстрее, inline умеренный).
FLAGS="-std=c++20 -g -O1 -pthread -Iinclude -isystem third_party -Itests"
CORE="src/core/Money.cpp src/core/Types.cpp src/core/Account.cpp src/core/Cashbox.cpp src/core/Operation.cpp"
CONFIG="src/config/ConfigLoader.cpp"
REPORTING="src/reporting/Logger.cpp"
ENGINE="src/engine/ServiceTimeProvider.cpp src/engine/ServiceStages.cpp src/engine/ClientFactory.cpp src/engine/SimulationRunner.cpp src/engine/AtmEngine.cpp"
SCENE="src/console/scene/GlyphSet.cpp src/console/scene/SceneCanvas.cpp src/console/scene/SceneSprites.cpp src/console/scene/SceneComposer.cpp src/console/scene/ScenePresenter.cpp src/console/scene/ActorAnim.cpp"
CONSOLE="src/console/CommandParser.cpp src/console/Terminal.cpp src/console/FrameDiffer.cpp src/console/LiveRenderer.cpp src/console/Signals.cpp src/console/SplashScreen.cpp src/console/AdminConsole.cpp $SCENE"
TEST_CONSOLE="src/console/CommandParser.cpp src/console/Terminal.cpp src/console/FrameDiffer.cpp src/console/LiveRenderer.cpp src/console/Signals.cpp src/console/SplashScreen.cpp $SCENE"
TESTS="tests/test_main.cpp tests/test_money.cpp tests/test_account.cpp tests/test_cashbox.cpp tests/test_operation.cpp tests/test_config.cpp tests/test_service_time.cpp tests/test_service_stages.cpp tests/test_simulation.cpp tests/test_engine.cpp tests/test_parser.cpp tests/test_renderer.cpp tests/test_scene_canvas.cpp tests/test_scene_compose.cpp tests/test_scene_presenter.cpp tests/test_frame_differ.cpp tests/test_splash.cpp tests/test_logger.cpp"

echo "== сборка -g без санитайзеров =="
g++ $FLAGS $CORE $CONFIG $REPORTING $ENGINE $TEST_CONSOLE $TESTS -o build/atmsim_tests_vg
g++ $FLAGS src/main.cpp $CORE $CONFIG $REPORTING $ENGINE $CONSOLE -o build/atm_sim_vg

# Быстрый конфиг для прогона приложения: те же поля, но крутим скорость, чтобы
# симуляция дала реальную работу за секунды (иначе под Valgrind это вечность).
sed -e 's/"arrival_rate_per_minute": [0-9.]*/"arrival_rate_per_minute": 12000/' \
    -e 's/"count": [0-9]*/"count": 60/' \
    -e 's/"time_scale": [0-9.]*/"time_scale": 200.0/' \
    config/default_config.json > build/vg_fast_config.json

# definite+indirect — реальные утечки (валят прогон через --error-exitcode=99).
# "still reachable"/"possibly lost" НЕ валят: у libstdc++ пула потоков и
# статических объектов это штатный шум, не наши баги.
VG="valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=all --track-origins=yes --errors-for-leak-kinds=definite,indirect --num-callers=20"

MEM_FAIL=0
# run_vg <ярлык> <команда...>: не роняет скрипт на логическом провале ребёнка,
# но помечает MEM_FAIL, если Valgrind нашёл ошибки памяти (код 99).
run_vg() {
    label="$1"; shift
    set +e
    "$@"
    rc=$?
    set -e
    if [ "$rc" = "99" ]; then
        echo "!! [$label] Valgrind ОБНАРУЖИЛ ошибки памяти/утечки (exit 99)"
        MEM_FAIL=1
    elif [ "$rc" = "0" ]; then
        echo "== [$label] чисто: память ok, программа завершилась 0"
    else
        echo "== [$label] память ok (Valgrind: 0 ошибок), программа вышла с кодом $rc — логика/тайминги под замедлением, НЕ память"
    fi
}

echo
echo "## memcheck: юнит-тесты (весь движок, потоки, рендер — детерминированно) ##"
run_vg "tests" $VG ./build/atmsim_tests_vg

echo
echo "## memcheck: atm_sim (реальный main + потоки; stdin открыт ~20с, потом EOF) ##"
# sleep держит пайп открытым: getline на главном потоке блокируется, фоновые
# потоки крутят симуляцию; по закрытии пайпа — EOF -> Quit -> join.
run_vg "atm_sim" sh -c '( sleep 20 ) | '"$VG"' ./build/atm_sim_vg build/vg_fast_config.json'

echo
if [ "$MEM_FAIL" = "0" ]; then
    echo "== Valgrind: ошибок памяти и утечек НЕ найдено =="
else
    echo "== Valgrind: НАЙДЕНЫ проблемы памяти (см. выше) =="; exit 1
fi
