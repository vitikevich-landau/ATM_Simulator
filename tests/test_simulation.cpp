#include "atmsim/engine/SimulationRunner.hpp"
#include "simple_test.hpp"

using namespace atmsim;

namespace {
// Базовая конфигурация для тестов симуляции (значения по умолчанию + мелкие правки).
Config baseConfig() {
    Config c;
    c.clients.count = 40;
    c.simulation.randomSeed = 12345;
    return c;
}
}  // namespace

// Ни один клиент не «теряется»: обслужен либо ушёл по терпению.
TEST(simulation_every_client_accounted_for) {
    const SimulationResult r = SimulationRunner::run(baseConfig());
    CHECK_EQ(r.totalClients, 40);
    CHECK_EQ(r.served + r.leftByPatience, r.totalClients);
}

// Инвариант сохранения денег держится и на полном прогоне симуляции.
TEST(simulation_conserves_money) {
    const SimulationResult r = SimulationRunner::run(baseConfig());
    CHECK(r.moneyConserved);
    CHECK_EQ(r.accountsEnd - r.cashEnd, r.accountsStart - r.cashStart);
}

// Один и тот же seed => полностью совпадающий результат (§5, детерминизм).
TEST(simulation_reproducible_with_same_seed) {
    const SimulationResult a = SimulationRunner::run(baseConfig());
    const SimulationResult b = SimulationRunner::run(baseConfig());
    CHECK_EQ(a.served, b.served);
    CHECK_EQ(a.leftByPatience, b.leftByPatience);
    CHECK_EQ(a.maxQueueLength, b.maxQueueLength);
    CHECK_EQ(a.cashEnd, b.cashEnd);
    CHECK_EQ(a.opFailed, b.opFailed);
}

// Перегрузка (частый приход, долгое обслуживание, малое терпение) => есть ушедшие.
TEST(simulation_reneging_happens_under_pressure) {
    Config c = baseConfig();
    c.clients.count = 60;
    c.clients.arrivalRatePerMinute = 30.0;
    c.clients.patienceSeconds = SecondsRange{5, 15};
    c.serviceTime.distribution = ServiceDistribution::Normal;
    c.serviceTime.meanSeconds = 30.0;
    c.serviceTime.stddevSeconds = 3.0;
    c.serviceTime.minSeconds = 10.0;

    const SimulationResult r = SimulationRunner::run(c);
    CHECK(r.leftByPatience > 0);
}

// Недогрузка (редкий приход, быстрое обслуживание, огромное терпение) => никто не уходит.
TEST(simulation_no_reneging_when_fast_and_patient) {
    Config c = baseConfig();
    c.clients.count = 30;
    c.clients.arrivalRatePerMinute = 1.0;
    c.clients.patienceSeconds = SecondsRange{100000, 100000};
    c.serviceTime.distribution = ServiceDistribution::Uniform;
    c.serviceTime.minSeconds = 1.0;
    c.serviceTime.maxSeconds = 3.0;

    const SimulationResult r = SimulationRunner::run(c);
    CHECK_EQ(r.leftByPatience, 0);
}
