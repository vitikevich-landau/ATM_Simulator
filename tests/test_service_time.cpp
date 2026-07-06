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
