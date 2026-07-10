#include <chrono>
#include <cmath>
#include <cstddef>
#include <set>
#include <thread>

#include "atmsim/engine/AtmEngine.hpp"
#include "atmsim/engine/ServiceStages.hpp"
#include "simple_test.hpp"

using namespace atmsim;
using namespace std::chrono_literals;

namespace {

// Все три типа операций — чтобы прогонять проверки планов в цикле.
const OperationType kAllOps[] = {OperationType::CheckBalance, OperationType::Withdraw,
                                 OperationType::Deposit};

// Порядковый номер этапа в плане операции (для проверки монотонности).
int stageIndex(OperationType op, ServiceStage st) {
    const auto& plan = serviceStagePlan(op);
    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].stage == st) return static_cast<int>(i);
    }
    return -1;
}

// Множество этапов плана (для проверки специфичных для операции шагов).
std::set<ServiceStage> stagesOf(OperationType op) {
    std::set<ServiceStage> st;
    for (const auto& s : serviceStagePlan(op)) st.insert(s.stage);
    return st;
}

}  // namespace

// Инвариант планов: доли этапов в сумме дают ровно 1.0 (иначе последний этап
// закончился бы раньше/позже конца обслуживания), каждая доля положительна,
// сценарий начинается вставкой карты и заканчивается её возвратом.
TEST(stage_plan_fractions_sum_to_one) {
    for (OperationType op : kAllOps) {
        const auto& plan = serviceStagePlan(op);
        CHECK(!plan.empty());
        double sum = 0.0;
        for (const auto& s : plan) {
            CHECK(s.fraction > 0.0);
            sum += s.fraction;
        }
        CHECK(std::fabs(sum - 1.0) < 1e-9);
        CHECK(plan.front().stage == ServiceStage::InsertCard);
        CHECK(plan.back().stage == ServiceStage::ReturnCard);
    }
}

// serviceStageAt: границы прижимаются (за пределами 0..1 — первый/последний
// этап), а с ростом прогресса этап не «откатывается назад» и все этапы плана
// достижимы.
TEST(stage_at_boundaries_and_monotonic_progression) {
    for (OperationType op : kAllOps) {
        CHECK(serviceStageAt(op, -0.5) == ServiceStage::InsertCard);
        CHECK(serviceStageAt(op, 0.0) == ServiceStage::InsertCard);
        CHECK(serviceStageAt(op, 1.0) == ServiceStage::ReturnCard);
        CHECK(serviceStageAt(op, 42.0) == ServiceStage::ReturnCard);

        const auto& plan = serviceStagePlan(op);
        int prev = 0;
        for (double p = 0.0; p <= 1.0; p += 0.001) {
            const int idx = stageIndex(op, serviceStageAt(op, p));
            CHECK(idx >= 0);         // этап всегда из плана этой операции
            CHECK(idx >= prev);      // и никогда не идёт назад
            prev = idx;
        }
        // К прогрессу 1.0 сценарий дошёл до последнего этапа.
        CHECK_EQ(prev, static_cast<int>(plan.size()) - 1);
    }
}

// Тематика этапов соответствует операции: купюры отсчитываются только при
// снятии, вкладываются и пересчитываются — только при внесении, экран с
// балансом — только у запроса баланса. Сумму вводят обе денежные операции.
TEST(stage_plans_are_operation_specific) {
    const auto check = stagesOf(OperationType::CheckBalance);
    const auto withdraw = stagesOf(OperationType::Withdraw);
    const auto deposit = stagesOf(OperationType::Deposit);

    CHECK(withdraw.count(ServiceStage::CountCash) == 1);
    CHECK(withdraw.count(ServiceStage::DispenseCash) == 1);
    CHECK(withdraw.count(ServiceStage::InsertCash) == 0);
    CHECK(withdraw.count(ServiceStage::ShowBalance) == 0);

    CHECK(deposit.count(ServiceStage::InsertCash) == 1);
    CHECK(deposit.count(ServiceStage::VerifyCash) == 1);
    CHECK(deposit.count(ServiceStage::CountCash) == 0);
    CHECK(deposit.count(ServiceStage::DispenseCash) == 0);

    CHECK(check.count(ServiceStage::ShowBalance) == 1);
    CHECK(check.count(ServiceStage::EnterAmount) == 0);
    CHECK(check.count(ServiceStage::CountCash) == 0);
    CHECK(check.count(ServiceStage::InsertCash) == 0);

    CHECK(withdraw.count(ServiceStage::EnterAmount) == 1);
    CHECK(deposit.count(ServiceStage::EnterAmount) == 1);
}

// У каждого этапа всех планов есть человекочитаемое название (не заглушка «?»),
// и оно не длиннее 19 колонок терминала: строка этапа на дашборде
// « └ имя [██████████] 100%» (3+имя+1+12+4) обязана влезать в левую колонку
// при её минимальной ширине 40, иначе fit() отрежет бар и процент.
TEST(stage_names_are_human_readable) {
    // Ширина в колонках = число символов UTF-8 (кириллица — 2 байта, 1 колонка):
    // считаем байты, не являющиеся продолжением последовательности (10xxxxxx).
    auto displayColumns = [](const std::string& s) {
        int cols = 0;
        for (const char ch : s) {
            if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++cols;
        }
        return cols;
    };
    for (OperationType op : kAllOps) {
        for (const auto& s : serviceStagePlan(op)) {
            const std::string name = to_string(s.stage);
            CHECK(!name.empty());
            CHECK(name != "?");
            CHECK(displayColumns(name) <= 19);
        }
    }
}

// Интеграция с движком: пока клиент обслуживается, снимок отдаёт этап и
// прогресс; прогресс растёт со временем, а на паузе замирает (§4.6); после
// remove клиента поля очищаются. Длительность обслуживания фиксируем uniform
// 60..60 с при time_scale = 1, чтобы за время теста обслуживание не кончилось.
TEST(engine_snapshot_exposes_service_stage_and_progress) {
    Config cfg;
    cfg.clients.count = 1;
    cfg.clients.arrivalRatePerMinute = 1000.0;               // клиент приходит сразу
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 60.0;
    cfg.serviceTime.maxSeconds = 60.0;
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    // Дожидаемся начала обслуживания.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }

    const AtmSnapshot s1 = engine.snapshot();
    CHECK(s1.currentClientId.has_value());
    CHECK(s1.currentStage.has_value());
    CHECK(std::fabs(s1.servicePlannedModelSec - 60.0) < 1e-9);
    CHECK(s1.serviceProgress >= 0.0);
    CHECK(s1.serviceProgress <= 1.0);

    // Прогресс непрерывен: чуть позже он строго больше (обслуживание идёт,
    // 60 c только началось — до 1.0 ещё далеко).
    std::this_thread::sleep_for(200ms);
    const AtmSnapshot s2 = engine.snapshot();
    CHECK(s2.serviceProgress > s1.serviceProgress);

    // Пауза замораживает прогресс вместе с таймером службы (§4.6): два снимка
    // с интервалом дают ОДИНАКОВУЮ долю, а этап продолжает показываться.
    // ВАЖНО про синхронизацию: requestPause() лишь выставляет режим и будит
    // поток обслуживания — поля прогресса замораживает САМ поток, когда
    // проснётся. Фиксированный sleep здесь был бы флейком (под TSan в Docker
    // поток может просыпаться заметно дольше), поэтому ждём заморозку
    // поллингом с дедлайном: прогресс перестал меняться => поток встал на
    // паузу (замороженные снимки вычисляются из одних и тех же полей и
    // совпадают бит-в-бит, так что сравнение double на равенство корректно).
    CHECK(engine.requestPause());
    AtmSnapshot p1{}, p2{};
    const auto pauseDeadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        p1 = engine.snapshot();
        std::this_thread::sleep_for(30ms);
        p2 = engine.snapshot();
        if (p1.serviceProgress == p2.serviceProgress) break;
        if (std::chrono::steady_clock::now() > pauseDeadline) break;
    }
    CHECK(p1.currentStage.has_value());
    CHECK_EQ(p1.serviceProgress, p2.serviceProgress);

    engine.requestResume();
    engine.requestStop();
    eng.join();
    arr.join();

    // После остановки движок доводит операцию до конца и сбрасывает прогресс.
    const AtmSnapshot done = engine.snapshot();
    CHECK(!done.currentClientId.has_value());
    CHECK(!done.currentStage.has_value());
    CHECK_EQ(done.serviceProgress, 0.0);
    CHECK_EQ(done.servicePlannedModelSec, 0.0);
}

// Арифметика прогресса при УСКОРЕННОМ времени (§4.7) — единственное место, где
// делится на time_scale: реальная длительность = модельная / time_scale. При
// time_scale = 10 обслуживание в 60 модельных секунд идёт 6 реальных: спустя
// ~1 реальную секунду прогресс ~1/6. Регресс «умножили вместо поделили» дал бы
// ~0.0017 — коридор [0.05, 0.9] надёжно различает и терпит медленную машину
// (сон может растянуться, но не до конца 6-секундного обслуживания).
TEST(engine_service_progress_respects_time_scale) {
    Config cfg;
    cfg.clients.count = 1;
    cfg.clients.arrivalRatePerMinute = 1000.0;               // клиент приходит сразу
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 60.0;
    cfg.serviceTime.maxSeconds = 60.0;
    cfg.simulation.timeScale = 10.0;
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().currentClientId.has_value());

    std::this_thread::sleep_for(1s);
    const AtmSnapshot s = engine.snapshot();
    // Длительность в снимке — МОДЕЛЬНАЯ, от time_scale не зависит.
    CHECK(std::fabs(s.servicePlannedModelSec - 60.0) < 1e-9);
    CHECK(s.serviceProgress > 0.05);
    CHECK(s.serviceProgress < 0.9);

    engine.requestStop();
    eng.join();
    arr.join();
}

// ПОДХОД к банкомату (clients.walk_seconds): пока клиент идёт, снимок отдаёт
// approaching/approachProgress, а этап ПУСТ — обслуживание не началось; пауза
// замораживает прогресс подхода (§4.6); когда клиент дошёл — поля подхода
// гаснут и появляется обычный этап с прогрессом обслуживания. Время подхода
// фиксируем 60/60 с, чтобы за время проверок клиент гарантированно «шёл».
TEST(engine_approach_phase_delays_service) {
    Config cfg;
    cfg.clients.count = 1;
    cfg.clients.arrivalRatePerMinute = 1000.0;               // клиент приходит сразу
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.clients.walkSeconds = WalkSecondsRange{60.0, 60.0};  // долгий подход
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 60.0;
    cfg.serviceTime.maxSeconds = 60.0;
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    // Дожидаемся, пока клиент будет взят из очереди (начнёт подход).
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }

    const AtmSnapshot s1 = engine.snapshot();
    CHECK(s1.currentClientId.has_value());
    CHECK(s1.approaching);
    CHECK(std::fabs(s1.approachPlannedModelSec - 60.0) < 1e-9);
    CHECK(s1.approachProgress >= 0.0);
    CHECK(s1.approachProgress <= 1.0);
    // Обслуживание НЕ началось: этапа нет, сервисный прогресс нулевой.
    CHECK(!s1.currentStage.has_value());
    CHECK_EQ(s1.serviceProgress, 0.0);
    CHECK_EQ(s1.servicePlannedModelSec, 0.0);

    // Прогресс подхода непрерывен: чуть позже он строго больше.
    std::this_thread::sleep_for(200ms);
    const AtmSnapshot s2 = engine.snapshot();
    CHECK(s2.approaching);
    CHECK(s2.approachProgress > s1.approachProgress);

    // Пауза замораживает подход, как и таймер службы (§4.6). Ждём заморозку
    // поллингом с дедлайном (см. комментарий в
    // engine_snapshot_exposes_service_stage_and_progress: фиксированный sleep
    // был бы флейком под TSan).
    CHECK(engine.requestPause());
    AtmSnapshot p1{}, p2{};
    const auto pauseDeadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        p1 = engine.snapshot();
        std::this_thread::sleep_for(30ms);
        p2 = engine.snapshot();
        if (p1.approachProgress == p2.approachProgress) break;
        if (std::chrono::steady_clock::now() > pauseDeadline) break;
    }
    CHECK(p1.approaching);
    CHECK_EQ(p1.approachProgress, p2.approachProgress);
    CHECK(!p1.currentStage.has_value());

    engine.requestResume();
    engine.requestStop();
    eng.join();
    arr.join();

    // Остановка прерывает подход, но операцию движок доводит до конца (§4.6:
    // клиент уже взят из очереди, начатое не бросаем) — клиент обслужен,
    // поля подхода и обслуживания сброшены.
    const AtmSnapshot done = engine.snapshot();
    CHECK(!done.currentClientId.has_value());
    CHECK(!done.approaching);
    CHECK_EQ(done.approachProgress, 0.0);
    CHECK_EQ(done.approachPlannedModelSec, 0.0);
    CHECK_EQ(done.totalServed, static_cast<std::uint64_t>(1));
}

// Подход завершается и уступает место обслуживанию: с коротким walk-временем
// вскоре после взятия клиента снимок показывает обычный этап, а поля подхода
// погашены. Заодно проверяется арифметика time_scale для подхода (та же, что
// у обслуживания: реальное время = модельное / time_scale): 10 модельных
// секунд подхода при time_scale = 20 — это 0.5 реальной секунды.
TEST(engine_approach_completes_and_service_starts) {
    Config cfg;
    cfg.clients.count = 1;
    cfg.clients.arrivalRatePerMinute = 1000.0;               // клиент приходит сразу
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.clients.walkSeconds = WalkSecondsRange{10.0, 10.0};
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 600.0;   // долгое обслуживание: не успеет кончиться
    cfg.serviceTime.maxSeconds = 600.0;
    cfg.simulation.timeScale = 20.0;
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    // Дожидаемся ПЕРЕХОДА к обслуживанию: появился этап — подход закончился.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentStage.has_value()) {
        std::this_thread::sleep_for(1ms);
    }

    const AtmSnapshot s = engine.snapshot();
    CHECK(s.currentClientId.has_value());
    CHECK(s.currentStage.has_value());
    CHECK(!s.approaching);
    CHECK_EQ(s.approachPlannedModelSec, 0.0);
    CHECK(std::fabs(s.servicePlannedModelSec - 600.0) < 1e-9);

    engine.requestStop();
    eng.join();
    arr.join();
}
