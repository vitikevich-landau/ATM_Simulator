#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
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
        std::thread eng([&] { engine.run(); });
        std::thread arr([&] { engine.generateArrivals(); });

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
        eng.join();
        arr.join();
    }
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
    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

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

// Отдельный поток-читатель (как render-поток дашборда) параллельно дёргает
// ВСЕ snapshot-методы, пока идёт обслуживание. Ценность теста — под TSan:
// путь чтения рендерера не должен создавать гонок данных (§4.8, §14).
TEST(engine_concurrent_readers_are_race_free) {
    const int count = 30;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    std::atomic<bool> stopReader{false};
    std::thread reader([&] {
        while (!stopReader.load()) {
            const auto s = engine.snapshot();
            const auto q = engine.queueSnapshot();
            const auto st = engine.statsSnapshot();
            const auto ops = engine.operations(OperationFilter{});
            const auto rep = engine.clientReport(1);
            (void)s; (void)q; (void)st; (void)ops; (void)rep;
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto s = engine.snapshot();
        if (s.totalServed + s.totalLeft >= static_cast<std::uint64_t>(count) &&
            s.queueLength == 0 && !s.currentClientId) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    stopReader.store(true);
    reader.join();
    engine.requestStop();
    eng.join();
    arr.join();
    CHECK(true);  // главный результат — чистый прогон под ThreadSanitizer
}

// РЕГРЕССИЯ: requestStop() ОБЯЗАН быстро завершить оба потока — и когда они спят
// в ожидании (долгое обслуживание / долгий интервал прихода), и во время
// «бессрочного» ТО (риск вечного сна или горячего цикла). Это единственный путь
// остановки после отказа от jthread/stop_token.
TEST(engine_request_stop_terminates_sleeping_threads) {
    Config cfg = fastConfig(5, 1.0);           // реальное время
    cfg.clients.arrivalRatePerMinute = 0.5;    // интервал ~120 c -> Arrival спит
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 300.0;        // обслуживание 300 c -> Engine спит
    cfg.serviceTime.maxSeconds = 300.0;
    cfg.clients.patienceSeconds = SecondsRange{100000, 100000};
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });
    std::this_thread::sleep_for(100ms);        // дать потокам заснуть в wait

    const auto t0 = std::chrono::steady_clock::now();
    engine.requestStop();
    eng.join();
    arr.join();
    CHECK(std::chrono::steady_clock::now() - t0 < 5s);
}

TEST(engine_request_stop_terminates_during_indefinite_maintenance) {
    Config cfg = fastConfig(0, 1.0);
    cfg.maintenance.defaultDurationSeconds = 0;  // 0 => ТО до явного stop (дедлайн = max)
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    engine.requestMaintenance(std::nullopt);
    std::this_thread::sleep_for(100ms);

    const auto t0 = std::chrono::steady_clock::now();
    engine.requestStop();
    eng.join();  // не должно ни зависнуть, ни крутиться в горячем цикле
    CHECK(std::chrono::steady_clock::now() - t0 < 5s);
}

// РЕГРЕССИЯ: после авто-завершения ТО по таймеру поток прихода должен
// проснуться и продолжить генерировать клиентов (arrivals_continue = false).
// Без notify_all при авто-завершении приход замирал навсегда.
TEST(engine_arrivals_resume_after_timed_maintenance) {
    Config cfg = fastConfig(30, 1000.0);
    cfg.maintenance.arrivalsContinue = false;  // во время ТО приход стоит и ЖДЁТ
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });
    std::this_thread::sleep_for(30ms);

    engine.requestMaintenance(std::optional<int>(2));  // 2 модельные сек ~ 2 мс

    // Ждём конца ТО.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().state == AtmState::Maintenance) {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(engine.snapshot().state != AtmState::Maintenance);

    // После конца ТО приход должен ОЖИТЬ: суммарное число известных системе
    // клиентов (в очереди + обслужено + ушло) обязано вырасти.
    const auto seen = [&] {
        const AtmSnapshot s = engine.snapshot();
        return s.totalServed + s.totalLeft + s.queueLength + (s.currentClientId ? 1u : 0u);
    };
    const auto before = seen();
    deadline = std::chrono::steady_clock::now() + 5s;
    bool grew = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (seen() > before) { grew = true; break; }
        std::this_thread::sleep_for(2ms);
    }
    CHECK(grew);

    engine.requestStop();
    eng.join();
    arr.join();
}

// maintenance start переводит в Maintenance, maintenance stop — обратно.
TEST(engine_maintenance_state_transitions) {
    Config cfg = fastConfig(0, 1.0);  // без клиентов — проверяем только режим
    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });

    engine.requestMaintenance(std::optional<int>(3600));  // долго, не авто-завершится
    std::this_thread::sleep_for(30ms);
    CHECK(engine.snapshot().state == AtmState::Maintenance);

    engine.endMaintenance();
    std::this_thread::sleep_for(30ms);
    CHECK(engine.snapshot().state != AtmState::Maintenance);

    engine.requestStop();
    eng.join();
    CHECK(engine.isStopped());
}

// При старте ТО клиенты из очереди уходят с вероятностью renege_probability.
// Ставим её в 1.0 — значит все стоящие в очереди должны уйти (§4.5).
TEST(engine_maintenance_reneges_queued_clients) {
    Config cfg = fastConfig(30, 1000.0);
    cfg.clients.arrivalRatePerMinute = 120.0;                 // приходят часто
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 40.0;                        // обслуживание долгое ->
    cfg.serviceTime.maxSeconds = 40.0;                        // очередь копится
    cfg.clients.patienceSeconds = SecondsRange{100000, 100000};  // по терпению не уходят
    cfg.maintenance.renegeProbability = 1.0;                  // при ТО уходят все из очереди
    cfg.maintenance.defaultDurationSeconds = 1000000;         // ТО долгое, чтобы наблюсти

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    // Ждём, пока в очереди накопится хотя бы 3 клиента.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && engine.snapshot().queueLength < 3) {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(engine.snapshot().queueLength >= 3);

    engine.requestMaintenance(std::nullopt);

    // Ждём, пока поток обслуживания применит решение «уйти/остаться».
    auto d2 = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < d2 &&
           engine.statsSnapshot().renegedByMaintenance == 0) {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(engine.statsSnapshot().renegedByMaintenance > 0);

    engine.requestStop();
    eng.join();
    arr.join();
}

// pause переводит банкомат в состояние Paused, resume — обратно.
TEST(engine_pause_and_resume_change_state) {
    Config cfg = fastConfig(0, 1.0);  // без клиентов — проверяем только режимы
    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });

    engine.requestPause();
    std::this_thread::sleep_for(20ms);
    CHECK(engine.snapshot().state == AtmState::Paused);

    engine.requestResume();
    std::this_thread::sleep_for(20ms);
    CHECK(engine.snapshot().state != AtmState::Paused);

    engine.requestStop();
    eng.join();
    CHECK(engine.isStopped());
}
