#include <chrono>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "atmsim/engine/AtmEngine.hpp"
#include "simple_test.hpp"

using namespace atmsim;
using namespace std::chrono_literals;

namespace {

Config fastConfig(int count, double timeScale) {
    Config c;
    c.clients.count = count;
    c.simulation.randomSeed = 7;
    c.simulation.timeScale = timeScale;  // >1 => симуляция бежит быстрее реального времени
    return c;
}

// Крутит движок до полного разбора очереди (или таймаута) и возвращает контроль.
// Возвращает true, если действительно доработали (а не упёрлись в таймаут).
bool runToCompletion(AtmEngine& engine, int count, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool done = false;
    {
        std::jthread eng([&](std::stop_token st) { engine.run(st); });
        std::jthread arr([&](std::stop_token st) { engine.generateArrivals(st); });

        while (std::chrono::steady_clock::now() < deadline) {
            const AtmSnapshot s = engine.snapshot();
            const bool allProcessed =
                s.totalServed + s.totalLeft >= static_cast<std::uint64_t>(count);
            if (allProcessed && s.queueLength == 0 && !s.currentClientId) {
                done = true;
                break;
            }
            std::this_thread::sleep_for(2ms);
        }
        engine.requestStop();
        eng.request_stop();
        arr.request_stop();
    }  // здесь потоки join'ятся
    return done;
}

}  // namespace

// Все клиенты в итоге либо обслужены, либо ушли по терпению — никто не потерян.
TEST(engine_processes_every_client) {
    const int count = 30;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);

    const bool done = runToCompletion(engine, count, 10s);
    CHECK(done);

    const AtmSnapshot s = engine.snapshot();
    CHECK_EQ(s.totalServed + s.totalLeft, static_cast<std::uint64_t>(count));
}

// Инвариант сохранения денег держится и в многопоточном прогоне.
TEST(engine_conserves_money_under_threads) {
    const int count = 40;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);

    const Money accountsStart = engine.accountsTotal();
    const Money cashStart = engine.snapshot().cashboxBalance;

    runToCompletion(engine, count, 10s);

    const Money accountsEnd = engine.accountsTotal();
    const Money cashEnd = engine.snapshot().cashboxBalance;
    // (счета − касса) должно сохраниться до копейки.
    CHECK_EQ(accountsEnd - cashEnd, accountsStart - cashStart);
}

// КЛЮЧЕВОЙ тест §14: stop применяется МГНОВЕННО, не дожидаясь окончания
// длинного обслуживания. Без прерываемого ожидания (§6.2) этот тест ждал бы
// ~100000 секунд; с ним — миллисекунды.
TEST(engine_stop_is_immediate_during_long_service) {
    Config cfg = fastConfig(1, 1.0);  // time_scale = 1 (реальное время)
    // Обслуживание клиента «длится» 100000 секунд.
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 100000.0;
    cfg.serviceTime.maxSeconds = 100000.0;
    // Терпение огромное — чтобы клиент не ушёл до начала обслуживания.
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};

    AtmEngine engine(cfg);
    std::jthread eng([&](std::stop_token st) { engine.run(st); });
    std::jthread arr([&](std::stop_token st) { engine.generateArrivals(st); });

    // Ждём, пока банкомат реально начнёт обслуживать клиента.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    bool serving = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.snapshot().currentClientId.has_value()) {
            serving = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(serving);

    // Замеряем время реакции на stop.
    const auto t0 = std::chrono::steady_clock::now();
    engine.requestStop();
    eng.request_stop();
    arr.request_stop();
    eng.join();
    arr.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // Должно остановиться за миллисекунды, а НЕ через 100000 секунд.
    CHECK(elapsed < 1s);
}

// В журнале ровно одна запись на каждого обслуженного клиента (§4.4).
TEST(engine_logs_one_record_per_served) {
    const int count = 30;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);
    runToCompletion(engine, count, 10s);

    const AtmSnapshot s = engine.snapshot();
    CHECK_EQ(engine.operations(OperationFilter{}).size(), static_cast<std::size_t>(s.totalServed));
}

// Отчёт по клиенту доступен, и его итоговый статус осмыслен.
TEST(engine_client_report_available_after_run) {
    const int count = 20;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);
    runToCompletion(engine, count, 10s);

    const auto rep = engine.clientReport(1);
    CHECK(rep.has_value());
    if (rep) {
        CHECK(rep->state == ClientState::Served || rep->state == ClientState::LeftQueue);
    }
    CHECK(!engine.clientReport(0).has_value());               // нет клиента #0
    CHECK(!engine.clientReport(count + 100).has_value());     // за пределами
}

// Фильтр operations --type возвращает только записи нужного типа.
TEST(engine_operations_filter_by_type) {
    const int count = 40;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);
    runToCompletion(engine, count, 10s);

    OperationFilter f;
    f.type = OperationType::Withdraw;
    for (const auto& r : engine.operations(f)) {
        CHECK(r.type == OperationType::Withdraw);
    }
}

// pause переводит банкомат в состояние Paused, resume — обратно.
TEST(engine_pause_and_resume_change_state) {
    Config cfg = fastConfig(0, 1.0);  // без клиентов — проверяем только режимы
    AtmEngine engine(cfg);
    std::jthread eng([&](std::stop_token st) { engine.run(st); });

    engine.requestPause();
    std::this_thread::sleep_for(20ms);
    CHECK(engine.snapshot().state == AtmState::Paused);

    engine.requestResume();
    std::this_thread::sleep_for(20ms);
    CHECK(engine.snapshot().state != AtmState::Paused);

    engine.requestStop();
    eng.request_stop();
    eng.join();
    CHECK(engine.isStopped());
}
