#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include "atmsim/engine/AtmEngine.hpp"
#include "atmsim/engine/ScopedEngineThreads.hpp"
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

// allClientsProcessed(): ложь до старта, истина после того, как все клиенты
// достигли терминального состояния (для панели «симуляция завершена»).
TEST(engine_reports_all_clients_processed_on_completion) {
    const int count = 20;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);

    CHECK(!engine.allClientsProcessed());  // ещё ничего не сгенерировано

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && !engine.allClientsProcessed()) {
        std::this_thread::sleep_for(2ms);
    }
    CHECK(engine.allClientsProcessed());
    const AtmSnapshot s = engine.snapshot();
    CHECK_EQ(s.totalServed + s.totalLeft, static_cast<std::uint64_t>(count));

    engine.requestStop();
    eng.join();
    arr.join();
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

    // РЕГРЕССИЯ (F1): прерванное обслуживание не должно приписывать банкомату
    // больше «занятого» времени, чем реально прошло. Клиента обслуживали лишь
    // доли секунды из номинальных 100000 c, поэтому загрузка обязана остаться
    // в [0, 1], а не взлететь до ~100000/uptime.
    CHECK(engine.statsSnapshot().utilization <= 1.0);
}

// pause ПРИОСТАНАВЛИВАЕТ текущее обслуживание, а не завершает его: клиент остаётся
// «в обслуживании» даже спустя время дольше realDuration, а resume доводит остаток
// до конца. Регрессия: раньше pause мгновенно «дообслуживал» текущего (как stop) —
// клиент исчезал из обслуживания при первой же паузе.
TEST(engine_pause_suspends_and_resumes_current_client) {
    Config cfg = fastConfig(1, 1.0);           // реальное время
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 1.0;          // realDuration = 1 c
    cfg.serviceTime.maxSeconds = 1.0;
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};  // не уйдёт по терпению

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    // Ждём, пока обслуживание реально началось.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().currentClientId.has_value());

    engine.requestPause();

    // Держим паузу ДОЛЬШЕ длительности обслуживания (realDuration = 1 c). Со старым
    // поведением (pause = «доработать текущую») клиент бы уже обслужился и пропал.
    std::this_thread::sleep_for(1500ms);
    {
        const AtmSnapshot s = engine.snapshot();
        CHECK(s.state == AtmState::Paused);
        CHECK(s.currentClientId.has_value());                    // всё ещё держим клиента
        CHECK_EQ(s.totalServed, static_cast<std::uint64_t>(0));  // и ещё не обслужили
    }

    // Возобновляем — остаток обслуживания обязан доработаться до конца.
    engine.requestResume();
    const auto deadline2 = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline2 &&
           engine.snapshot().totalServed == 0) {
        std::this_thread::sleep_for(2ms);
    }
    CHECK_EQ(engine.snapshot().totalServed, static_cast<std::uint64_t>(1));

    engine.requestStop();
    eng.join();
    arr.join();
}

// maintenance start НЕ обрывает текущего клиента и не "докидывает" операцию
// мгновенно. Команда принимается сразу, но режим ТО начинается только после
// штатного окончания текущего обслуживания (§4.5).
TEST(engine_maintenance_waits_for_current_service_to_finish) {
    Config cfg = fastConfig(2, 1.0);           // реальное время
    cfg.clients.arrivalRatePerMinute = 1000.0; // второй клиент быстро встанет в очередь
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 1.0;          // обслуживание длится 1 c
    cfg.serviceTime.maxSeconds = 1.0;
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.maintenance.renegeProbability = 0.0;   // очередь остаётся ждать ТО

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().currentClientId.has_value());

    // Команда при живом клиенте отвечает «отложено» (консоль печатает точный ответ).
    CHECK(engine.requestMaintenance(std::optional<int>(3600)) == MaintenanceStart::Deferred);

    // Через долю секунды клиент всё ещё должен обслуживаться. Старое поведение
    // мгновенно применяло операцию и сбрасывало currentClient_ прямо на команде ТО.
    std::this_thread::sleep_for(150ms);
    {
        const AtmSnapshot s = engine.snapshot();
        CHECK(s.currentClientId.has_value());
        CHECK_EQ(s.totalServed, static_cast<std::uint64_t>(0));
        CHECK(s.state == AtmState::Serving);
        // Снимок сообщает «ТО запрошено, дорабатываем клиента» — статус для UI.
        CHECK(s.maintenancePending);
    }

    // После штатной секунды обслуживания банкомат входит в ТО и не берёт второго
    // клиента из очереди, пока ТО не завершится.
    auto d2 = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < d2) {
        const AtmSnapshot s = engine.snapshot();
        if (s.state == AtmState::Maintenance && !s.currentClientId) break;
        std::this_thread::sleep_for(2ms);
    }
    {
        const AtmSnapshot s = engine.snapshot();
        CHECK(s.state == AtmState::Maintenance);
        CHECK(!s.currentClientId.has_value());
        CHECK_EQ(s.totalServed, static_cast<std::uint64_t>(1));
        // ТО реально началось — флаг «отложено» обязан сняться.
        CHECK(!s.maintenancePending);
    }

    std::this_thread::sleep_for(150ms);
    CHECK_EQ(engine.snapshot().totalServed, static_cast<std::uint64_t>(1));

    engine.requestStop();
    eng.join();
    arr.join();
}

// Повторный «maintenance start» во время идущего ТО (продление) обязан заново
// применить решение «уйти/остаться» (§4.5 п.3) к накопившейся очереди. Регресс:
// флаг взводился, но поток обслуживания сидел во внутреннем цикле ТО и до
// проверки на входе в ветку не доходил — решение молча пропускалось.
TEST(engine_maintenance_restart_reapplies_reneging) {
    const int count = 10;
    Config cfg = fastConfig(count, 1000.0);
    cfg.clients.arrivalRatePerMinute = 6000.0;                     // прибывают мгновенно
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};  // терпение не мешает
    cfg.maintenance.renegeProbability = 1.0;                       // при старте ТО уходят ВСЕ
    cfg.maintenance.arrivalsContinue = true;

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });

    // ТО стартует при ПУСТОЙ очереди — первое решение «уйти/остаться» никого не застаёт.
    CHECK(engine.requestMaintenance(std::optional<int>(1000000)) == MaintenanceStart::Started);
    {
        const auto d = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < d &&
               engine.snapshot().state != AtmState::Maintenance) {
            std::this_thread::sleep_for(1ms);
        }
    }
    CHECK(engine.snapshot().state == AtmState::Maintenance);

    // Все клиенты приходят уже ВО ВРЕМЯ ТО и копятся в очереди (терпение огромное,
    // обслуживания нет — никто не убывает).
    std::thread arr([&] { engine.generateArrivals(); });
    arr.join();
    CHECK_EQ(engine.snapshot().queueLength, static_cast<std::size_t>(count));

    // Продление ТО: с renege_probability=1.0 вся очередь обязана уйти сразу.
    CHECK(engine.requestMaintenance(std::optional<int>(1000000)) == MaintenanceStart::Started);
    {
        const auto d = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < d && engine.snapshot().queueLength != 0) {
            std::this_thread::sleep_for(1ms);
        }
    }
    const AtmSnapshot s = engine.snapshot();
    CHECK_EQ(s.queueLength, static_cast<std::size_t>(0));
    CHECK(s.state == AtmState::Maintenance);  // ТО продолжается, банкомат не «ожил»
    CHECK_EQ(engine.statsSnapshot().renegedByMaintenance, static_cast<std::uint64_t>(count));

    engine.requestStop();
    eng.join();
}

// Клиент должен уходить ровно из очереди, даже если банкомат занят длинной
// операцией другого клиента. Раньше reneging проверялся только при pop_front().
TEST(engine_reneges_queued_clients_while_service_busy) {
    Config cfg = fastConfig(2, 100.0);
    cfg.clients.arrivalMode = ArrivalMode::Batch;
    cfg.clients.arrivalRatePerMinute = 6000.0;       // второй приходит почти сразу
    cfg.clients.patienceSeconds = SecondsRange{100, 100}; // 1 c реального времени
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 300.0;              // 3 c реального обслуживания
    cfg.serviceTime.maxSeconds = 300.0;

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().currentClientId.has_value());

    deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().queueLength == 0) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().queueLength > 0);

    deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().totalLeft == 0) {
        std::this_thread::sleep_for(2ms);
    }
    {
        const AtmSnapshot s = engine.snapshot();
        CHECK_EQ(s.totalLeft, static_cast<std::uint64_t>(1));
        CHECK_EQ(s.queueLength, static_cast<std::size_t>(0));
        CHECK(s.currentClientId.has_value());                    // первый ещё обслуживается
        CHECK_EQ(s.totalServed, static_cast<std::uint64_t>(0));
    }

    engine.requestStop();
    eng.join();
    arr.join();
}

// На паузе очередь продолжает жить: новые клиенты встают, но терпение всё равно
// тикает, и они уходят без ожидания resume.
TEST(engine_reneges_queued_clients_while_paused) {
    Config cfg = fastConfig(1, 100.0);
    cfg.clients.patienceSeconds = SecondsRange{50, 50};  // 0.5 c реального времени

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });
    CHECK(engine.requestPause());
    std::thread arr([&] { engine.generateArrivals(); });

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().queueLength == 0) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK_EQ(engine.snapshot().queueLength, static_cast<std::size_t>(1));

    deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().totalLeft == 0) {
        std::this_thread::sleep_for(2ms);
    }
    {
        const AtmSnapshot s = engine.snapshot();
        CHECK(s.state == AtmState::Paused);
        CHECK_EQ(s.totalLeft, static_cast<std::uint64_t>(1));
        CHECK_EQ(s.queueLength, static_cast<std::size_t>(0));
    }

    engine.requestStop();
    eng.join();
    arr.join();
}

// Во время ТО оставшиеся ждать клиенты не висят до maintenance stop: если терпение
// истекло раньше, они уходят из очереди по обычному reneging.
TEST(engine_reneges_queued_clients_during_maintenance) {
    Config cfg = fastConfig(1, 100.0);
    cfg.clients.patienceSeconds = SecondsRange{50, 50}; // 0.5 c реального времени
    cfg.maintenance.renegeProbability = 0.0;            // при старте ТО клиент остаётся ждать
    cfg.maintenance.defaultDurationSeconds = 100000;    // ТО долгое
    cfg.maintenance.arrivalsContinue = true;

    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });
    engine.requestMaintenance(std::nullopt);
    std::thread arr([&] { engine.generateArrivals(); });

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().queueLength == 0) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK_EQ(engine.snapshot().queueLength, static_cast<std::size_t>(1));

    deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           engine.snapshot().totalLeft == 0) {
        std::this_thread::sleep_for(2ms);
    }
    {
        const AtmSnapshot s = engine.snapshot();
        CHECK(s.state == AtmState::Maintenance);
        CHECK_EQ(s.totalLeft, static_cast<std::uint64_t>(1));
        CHECK_EQ(s.queueLength, static_cast<std::size_t>(0));
    }

    engine.requestStop();
    eng.join();
    arr.join();
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

// Фильтр operations --last N возвращает РОВНО последние N записей журнала в
// хронологическом порядке (новые в конце) — тот же результат, что «взять всё и
// оставить хвост». Регресс на оптимизацию обхода журнала с конца (горячий путь
// ленты дашборда): порядок и состав хвоста не должны измениться. Заодно —
// комбинация type + last.
TEST(engine_operations_last_returns_tail_in_order) {
    const int count = 60;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);
    runToCompletion(engine, count, 10s);

    const std::vector<OperationRecord> all = engine.operations(OperationFilter{});
    CHECK(all.size() >= 10);  // достаточно записей, чтобы хвост был осмыслен

    // last = 5 совпадает с последними 5 записями полного журнала, id-в-id.
    OperationFilter f5;
    f5.last = 5;
    const std::vector<OperationRecord> tail5 = engine.operations(f5);
    CHECK_EQ(tail5.size(), static_cast<std::size_t>(5));
    for (std::size_t i = 0; i < tail5.size(); ++i) {
        const OperationRecord& expected = all[all.size() - 5 + i];
        CHECK_EQ(tail5[i].id, expected.id);
    }

    // last больше журнала — возвращается весь журнал без обрезки/дублей.
    OperationFilter fbig;
    fbig.last = all.size() + 100;
    CHECK_EQ(engine.operations(fbig).size(), all.size());

    // type + last: последние N записей ИМЕННО этого типа, порядок сохранён.
    OperationFilter ft;
    ft.type = OperationType::Withdraw;
    const std::vector<OperationRecord> allWithdraw = engine.operations(ft);
    ft.last = 3;
    const std::vector<OperationRecord> tailWithdraw = engine.operations(ft);
    CHECK(tailWithdraw.size() <= 3);
    for (const auto& r : tailWithdraw) CHECK(r.type == OperationType::Withdraw);
    if (allWithdraw.size() >= 3) {
        CHECK_EQ(tailWithdraw.size(), static_cast<std::size_t>(3));
        for (std::size_t i = 0; i < tailWithdraw.size(); ++i) {
            CHECK_EQ(tailWithdraw[i].id, allWithdraw[allWithdraw.size() - 3 + i].id);
        }
    }
}

// fullSnapshot() под одним локом отдаёт то же, что пять раздельных снимков.
// Проверяем на отработавшем движке (состояние стабильно, потоки не мутируют).
TEST(engine_full_snapshot_matches_parts) {
    const int count = 40;
    Config cfg = fastConfig(count, 1000.0);
    AtmEngine engine(cfg);
    runToCompletion(engine, count, 10s);

    OperationFilter feed;
    feed.last = 5;
    const FullSnapshot full = engine.fullSnapshot(feed);

    const AtmSnapshot atm = engine.snapshot();
    const StatsSnapshot st = engine.statsSnapshot();
    const std::vector<ClientSnapshot> q = engine.queueSnapshot();
    const std::vector<OperationRecord> ops = engine.operations(feed);

    CHECK_EQ(full.atm.totalServed, atm.totalServed);
    CHECK_EQ(full.atm.queueLength, atm.queueLength);
    CHECK_EQ(full.stats.served, st.served);
    CHECK_EQ(full.queue.size(), q.size());
    CHECK_EQ(full.ops.size(), ops.size());
    CHECK(full.allProcessed == engine.allClientsProcessed());
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

// Регресс (P1): pause во время ТО не должен обрывать техобслуживание. Раньше
// pause переводил Maintenance -> Paused, а resume уводил в Idle/Serving в обход
// таймера и команды maintenance stop — ТО завершалось досрочно (нарушение §4.5
// п.5). Теперь pause во время ТО — no-op, и выйти из ТО можно только штатно.
TEST(engine_pause_during_maintenance_is_ignored) {
    Config cfg = fastConfig(0, 1.0);  // без клиентов — проверяем только режим
    AtmEngine engine(cfg);
    std::thread eng([&] { engine.run(); });

    // Никого не обслуживаем — ТО начинается сразу (Started), без флага «отложено».
    CHECK(engine.requestMaintenance(std::optional<int>(3600)) == MaintenanceStart::Started);
    std::this_thread::sleep_for(30ms);
    CHECK(engine.snapshot().state == AtmState::Maintenance);
    CHECK(!engine.snapshot().maintenancePending);

    // Пауза во время ТО игнорируется: команда возвращает false, режим не меняется.
    CHECK(!engine.requestPause());
    std::this_thread::sleep_for(30ms);
    CHECK(engine.snapshot().state == AtmState::Maintenance);

    // И resume не вытаскивает из ТО — режим по-прежнему Maintenance.
    engine.requestResume();
    std::this_thread::sleep_for(30ms);
    CHECK(engine.snapshot().state == AtmState::Maintenance);

    // Штатный выход из ТО (maintenance stop) по-прежнему работает.
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

// ScopedEngineThreads (RAII): stop() останавливает и присоединяет оба потока
// движка, после чего движок в состоянии Stopped; повторный stop() безопасен.
TEST(scoped_engine_threads_stop_is_idempotent) {
    Config cfg = fastConfig(0, 1.0);  // без клиентов — проверяем только жизненный цикл
    AtmEngine engine(cfg);
    {
        ScopedEngineThreads threads(engine);
        std::this_thread::sleep_for(20ms);  // дать потокам стартовать
        CHECK(!engine.isStopped());
        threads.stop();                     // явная остановка + join
        CHECK(engine.isStopped());
        threads.stop();                     // идемпотентно — не должно зависнуть/упасть
    }
    CHECK(engine.isStopped());
}

// RAII-путь без явного stop(): деструктор обязан сам остановить и присоединить
// потоки (иначе тест завис бы на join незавершённого потока обслуживания).
TEST(scoped_engine_threads_destructor_joins) {
    Config cfg = fastConfig(0, 1.0);
    AtmEngine engine(cfg);
    {
        ScopedEngineThreads threads(engine);
        std::this_thread::sleep_for(20ms);
    }  // ~ScopedEngineThreads(): requestStop() + join()
    CHECK(engine.isStopped());
}
