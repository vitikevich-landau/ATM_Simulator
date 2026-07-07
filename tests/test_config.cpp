#include <string>

#include "atmsim/config/ConfigLoader.hpp"
#include "simple_test.hpp"

using namespace atmsim;

// Полная конфигурация из строки читается в правильные поля.
TEST(config_reads_all_fields) {
    const std::string txt = R"({
      "atm": { "initial_cash": 50000000, "low_cash_threshold": 2000000, "currency": "EUR" },
      "clients": {
        "arrival_mode": "poisson",
        "arrival_rate_per_minute": 4.0,
        "count": 50,
        "initial_balance": 100000,
        "operation_weights": { "check_balance": 0.3, "withdraw": 0.5, "deposit": 0.2 },
        "amount_range": { "min": 50000, "max": 3000000 },
        "patience_seconds": { "min": 30, "max": 300 }
      },
      "service_time": { "distribution": "normal",
        "params": { "mean_seconds": 25, "stddev_seconds": 7, "min_seconds": 5 } },
      "simulation": { "random_seed": 42, "time_scale": 1.0 },
      "ui": { "live_mode": true, "refresh_hz": 4 }
    })";

    const Config c = ConfigLoader::loadFromString(txt);
    CHECK_EQ(c.atm.initialCash, Money(50000000));
    CHECK_EQ(c.atm.lowCashThreshold, Money(2000000));
    CHECK_EQ(c.atm.currency, std::string("EUR"));
    CHECK(c.clients.arrivalMode == ArrivalMode::Poisson);
    CHECK_EQ(c.clients.count, 50);
    CHECK_EQ(c.clients.amountRange.max, Money(3000000));
    CHECK(c.serviceTime.distribution == ServiceDistribution::Normal);
    CHECK_EQ(c.simulation.randomSeed, static_cast<std::uint64_t>(42));
    CHECK(c.ui.liveMode == true);
}

// Комментарии //… и /*…*/ в конфиге допускаются (для документирования файла) и
// не мешают чтению значений.
TEST(config_allows_comments) {
    const std::string txt = R"({
      // строчный комментарий
      "atm": { "currency": "USD" },   // валюта
      /* блочный
         комментарий */
      "clients": { "count": 7 }
    })";
    const Config c = ConfigLoader::loadFromString(txt);
    CHECK_EQ(c.atm.currency, std::string("USD"));
    CHECK_EQ(c.clients.count, 7);
}

// Отсутствующие ключи -> остаются значения по умолчанию (из Config.hpp).
TEST(config_uses_defaults_for_missing_keys) {
    const Config c = ConfigLoader::loadFromString("{}");
    CHECK_EQ(c.ui.refreshHz, 4);
    CHECK(c.ui.liveMode == true);
    CHECK(c.clients.arrivalMode == ArrivalMode::Poisson);
    CHECK_EQ(c.atm.currency, std::string("EUR"));
}

// Недопустимый режим прихода -> ConfigError.
TEST(config_rejects_unknown_arrival_mode) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"clients": {"arrival_mode": "nope"}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Синтаксически битый JSON -> ConfigError.
TEST(config_rejects_malformed_json) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString("{ this is not json ");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Нарушение согласованности (min > max) -> ConfigError.
TEST(config_rejects_inverted_amount_range) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"clients": {"amount_range": {"min": 100, "max": 10}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// amount_range.min == 0 недопустим: сумма 0 в снятии/внесении даёт InvalidAmount,
// то есть гарантированно проваленную операцию (нужно 1 <= min).
TEST(config_rejects_zero_amount_min) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"clients": {"amount_range": {"min": 0, "max": 1000}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Отрицательный stddev недопустим при любом распределении (уходит в
// std::normal_distribution, precondition которого stddev > 0).
TEST(config_rejects_negative_stddev) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(
            R"({"service_time": {"distribution": "normal",
                "params": {"mean_seconds": 25, "stddev_seconds": -1}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Для normal нужен СТРОГО положительный stddev — ноль тоже отвергаем.
TEST(config_rejects_zero_stddev_for_normal) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(
            R"({"service_time": {"distribution": "normal",
                "params": {"mean_seconds": 25, "stddev_seconds": 0}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// renege_probability вне [0, 1] недопустимо (уходит в bernoulli_distribution).
TEST(config_rejects_renege_probability_out_of_range) {
    bool threwHigh = false;
    try {
        ConfigLoader::loadFromString(R"({"maintenance": {"renege_probability": 1.5}})");
    } catch (const ConfigError&) {
        threwHigh = true;
    }
    CHECK(threwHigh);

    bool threwLow = false;
    try {
        ConfigLoader::loadFromString(R"({"maintenance": {"renege_probability": -0.1}})");
    } catch (const ConfigError&) {
        threwLow = true;
    }
    CHECK(threwLow);
}

// Границы диапазона (0.0 и 1.0) допустимы и не бросают.
TEST(config_accepts_boundary_renege_probability) {
    const Config lo = ConfigLoader::loadFromString(R"({"maintenance": {"renege_probability": 0.0}})");
    CHECK(lo.maintenance.renegeProbability == 0.0);
    const Config hi = ConfigLoader::loadFromString(R"({"maintenance": {"renege_probability": 1.0}})");
    CHECK(hi.maintenance.renegeProbability == 1.0);
}
