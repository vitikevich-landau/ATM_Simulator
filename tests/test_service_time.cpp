#include <random>

#include "atmsim/engine/ServiceTimeProvider.hpp"
#include "simple_test.hpp"

using namespace atmsim;

TEST(service_time_uniform_stays_in_range) {
    ServiceTimeConfig cfg;
    cfg.distribution = ServiceDistribution::Uniform;
    cfg.minSeconds = 5.0;
    cfg.maxSeconds = 15.0;
    auto p = makeServiceTimeProvider(cfg);

    std::mt19937_64 rng(1);
    for (int i = 0; i < 1000; ++i) {
        const double v = p->nextSeconds(OperationType::Withdraw, rng);
        CHECK(v >= 5.0 && v <= 15.0);
    }
}

TEST(service_time_normal_never_below_min) {
    ServiceTimeConfig cfg;
    cfg.distribution = ServiceDistribution::Normal;
    cfg.meanSeconds = 25.0;
    cfg.stddevSeconds = 7.0;
    cfg.minSeconds = 5.0;
    auto p = makeServiceTimeProvider(cfg);

    std::mt19937_64 rng(2);
    for (int i = 0; i < 1000; ++i) {
        CHECK(p->nextSeconds(OperationType::Deposit, rng) >= 5.0);
    }
}

// expectedServiceSeconds: среднее время обслуживания (для ρ = λ/μ) зависит от
// РАСПРЕДЕЛЕНИЯ. Для uniform это середина диапазона, а не mean_seconds.
TEST(service_time_expected_mean_by_distribution) {
    ServiceTimeConfig uni;
    uni.distribution = ServiceDistribution::Uniform;
    uni.minSeconds = 5.0;
    uni.maxSeconds = 15.0;
    uni.meanSeconds = 999.0;  // для uniform НЕ должно использоваться
    CHECK_EQ(expectedServiceSeconds(uni), 10.0);  // (5 + 15) / 2, не 999

    ServiceTimeConfig nrm;
    nrm.distribution = ServiceDistribution::Normal;
    nrm.meanSeconds = 25.0;
    CHECK_EQ(expectedServiceSeconds(nrm), 25.0);

    ServiceTimeConfig exp;
    exp.distribution = ServiceDistribution::Exponential;
    exp.meanSeconds = 8.0;
    CHECK_EQ(expectedServiceSeconds(exp), 8.0);
}

TEST(service_time_reproducible_with_same_seed) {
    ServiceTimeConfig cfg;  // normal по умолчанию
    auto p1 = makeServiceTimeProvider(cfg);
    auto p2 = makeServiceTimeProvider(cfg);

    std::mt19937_64 r1(42), r2(42);
    for (int i = 0; i < 100; ++i) {
        CHECK_EQ(p1->nextSeconds(OperationType::CheckBalance, r1),
                 p2->nextSeconds(OperationType::CheckBalance, r2));
    }
}
