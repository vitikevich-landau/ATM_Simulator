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

// currency_format переопределяет формат валюты поверх встроенной таблицы:
// заданные поля перекрывают, незаданные берутся из таблицы по коду валюты.
TEST(config_reads_currency_format_override) {
    const Config c = ConfigLoader::loadFromString(R"({
      "atm": { "currency": "USD", "currency_format": {
        "symbol": "$", "symbol_position": "after", "space_between": true,
        "group_separator": " ", "decimal_separator": ",", "decimals": 3 } }
    })");
    CHECK(c.atm.currencyOverride.symbolBefore.has_value());
    CHECK(*c.atm.currencyOverride.symbolBefore == false);   // "after"
    CHECK(c.atm.currencyOverride.decimals.has_value());
    CHECK_EQ(*c.atm.currencyOverride.decimals, 3);
    const CurrencyFormat f = resolveCurrencyFormat(c.atm.currency, c.atm.currencyOverride);
    CHECK_EQ(formatMoney(123'456, f), std::string("1 234,560 $"));  // группа " ", десятичная ",", 3 знака
}

// decimals вне 0..6 -> ConfigError (иначе строка денег раздулась бы нулями).
TEST(config_rejects_bad_currency_decimals) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"atm": {"currency_format": {"decimals": 9}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Round-trip: реальный config/default_config.json грузится через ConfigLoader и
// доезжает до полей. Проверяем НЕ-дефолтные значения: если ключ переименуют в
// одном месте (загрузчик или файл), value(key, default) молча вернул бы дефолт —
// и этот тест упал бы ГРОМКО, а не рассинхронился незаметно. Заодно ловит битый
// JSON в файле (например, висячую запятую после правки комментариев).
TEST(config_default_file_round_trips) {
    const Config c = ConfigLoader::loadFromFile("config/default_config.json");
    CHECK_EQ(c.clients.count, 1000);                    // != дефолт 50
    CHECK_EQ(c.clients.arrivalRatePerMinute, 20.0);     // != дефолт 4.0
    CHECK_EQ(c.clients.amountRange.max, Money(300000)); // != дефолт 3000000
    CHECK_EQ(c.clients.walkSeconds.max, 3.0);           // != дефолт 0.0
    CHECK_EQ(c.serviceTime.meanSeconds, 5.0);           // != дефолт 25.0
    CHECK(c.ui.scene == true);                          // != дефолт false
    CHECK_EQ(c.atm.currency, std::string("EUR"));
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

// walk_seconds (время подхода клиента к банкомату) читается; по умолчанию
// 0/0 — подход мгновенный (поведение до фичи, чтобы программные конфиги и
// тесты не меняли семантику незаметно; «живое» значение — в default_config.json).
TEST(config_reads_walk_seconds) {
    const Config def = ConfigLoader::loadFromString("{}");
    CHECK_EQ(def.clients.walkSeconds.min, 0.0);
    CHECK_EQ(def.clients.walkSeconds.max, 0.0);

    const Config c = ConfigLoader::loadFromString(
        R"({"clients": {"walk_seconds": {"min": 1.5, "max": 3.0}}})");
    CHECK_EQ(c.clients.walkSeconds.min, 1.5);
    CHECK_EQ(c.clients.walkSeconds.max, 3.0);
}

// walk_seconds: отрицательное время и min > max — ошибки конфигурации
// (диапазон уходит в uniform_real_distribution, precondition которого min <= max).
TEST(config_rejects_bad_walk_seconds) {
    bool threwNegative = false;
    try {
        ConfigLoader::loadFromString(
            R"({"clients": {"walk_seconds": {"min": -1.0, "max": 2.0}}})");
    } catch (const ConfigError&) {
        threwNegative = true;
    }
    CHECK(threwNegative);

    bool threwInverted = false;
    try {
        ConfigLoader::loadFromString(
            R"({"clients": {"walk_seconds": {"min": 3.0, "max": 1.0}}})");
    } catch (const ConfigError&) {
        threwInverted = true;
    }
    CHECK(threwInverted);
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

// Абсурдно большой count уронил бы vector::reserve (bad_alloc/length_error) в
// std::terminate вместо ConfigError -> проверяем верхнюю границу.
TEST(config_rejects_too_large_count) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"clients": {"count": 1000000000}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Отрицательный начальный баланс завёл бы счёт «в минусе» -> ConfigError.
TEST(config_rejects_negative_initial_balance) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"clients": {"initial_balance": -100}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// mean_seconds == 0 для exponential молча подменялся λ=1 и обнулял ρ -> теперь
// ConfigError.
TEST(config_rejects_zero_mean_for_exponential) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(
            R"({"service_time": {"distribution": "exponential",
                "params": {"mean_seconds": 0}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// То же требование mean_seconds > 0 действует и для normal.
TEST(config_rejects_zero_mean_for_normal) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(
            R"({"service_time": {"distribution": "normal",
                "params": {"mean_seconds": 0, "stddev_seconds": 7}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// refresh_hz > 1000 обнулил бы период перерисовки (1000/hz мс) — циклы отрисовки
// крутились бы без сна. Верхняя граница 60 отсекает такой конфиг ещё на загрузке.
TEST(config_rejects_too_large_refresh_hz) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"ui": {"refresh_hz": 2000}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
    // Граница диапазона включительно: 60 — валидно.
    const Config c = ConfigLoader::loadFromString(R"({"ui": {"refresh_hz": 60}})");
    CHECK_EQ(c.ui.refreshHz, 60);
}

// Переполнение числового литерала («3.0e999» -> json::out_of_range, который НЕ
// parse_error) тоже должно приходить наружу как ConfigError, а не как
// «непредвиденная ошибка» в main.
TEST(config_rejects_overflowing_number_as_config_error) {
    bool threw = false;
    try {
        ConfigLoader::loadFromString(
            R"({"clients": {"walk_seconds": {"min": 1.5, "max": 3.0e999}}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}
