#include "atmsim/core/Money.hpp"
#include "simple_test.hpp"

using namespace atmsim;

TEST(money_format_positive) {
    CHECK_EQ(formatMoney(50000), std::string("500.00"));
    CHECK_EQ(formatMoney(134000), std::string("1340.00"));
    CHECK_EQ(formatMoney(5), std::string("0.05"));
    CHECK_EQ(formatMoney(0), std::string("0.00"));
    CHECK_EQ(formatMoney(1), std::string("0.01"));
}

TEST(money_format_negative) {
    CHECK_EQ(formatMoney(-5), std::string("-0.05"));
    CHECK_EQ(formatMoney(-134000), std::string("-1340.00"));
}

// Локализованный вывод по встроенной таблице валют: символ и его позиция,
// группировка разрядов, десятичный знак.
TEST(money_format_currency_builtin) {
    // EUR: символ после числа с пробелом, группировка пробелом, десятичный '.'.
    const CurrencyFormat eur = builtinCurrencyFormat("EUR");
    CHECK_EQ(formatMoney(50'000'000, eur), std::string("500 000.00 €"));
    CHECK_EQ(formatMoney(123'456, eur), std::string("1 234.56 €"));
    CHECK_EQ(formatMoney(5, eur), std::string("0.05 €"));
    CHECK_EQ(formatMoney(-123'456, eur), std::string("-1 234.56 €"));

    // USD: символ перед числом без пробела, группировка запятой.
    const CurrencyFormat usd = builtinCurrencyFormat("USD");
    CHECK_EQ(formatMoney(50'000'000, usd), std::string("$500,000.00"));
    CHECK_EQ(formatMoney(-500, usd), std::string("-$5.00"));

    // JPY: без дробной части (decimals=0), символ спереди, группировка запятой.
    const CurrencyFormat jpy = builtinCurrencyFormat("JPY");
    CHECK_EQ(formatMoney(123'456, jpy), std::string("¥1,234"));

    // Неизвестный код -> сам код как токен, группировка пробелом.
    const CurrencyFormat xxx = builtinCurrencyFormat("XYZ");
    CHECK_EQ(formatMoney(100'000, xxx), std::string("1 000.00 XYZ"));

    // withCurrency=false -> только число с группировкой, без токена валюты.
    CHECK_EQ(formatMoney(50'000'000, eur, false), std::string("500 000.00"));
}

// Переопределение поверх встроенной таблицы (atm.currency_format).
TEST(money_format_currency_override) {
    // Русская локализация RUB: десятичная запятая, без группировки разрядов.
    CurrencyOverride ru;
    ru.decimalSeparator = std::string(",");
    ru.groupSeparator = std::string("");   // "" — без группировки
    const CurrencyFormat rub = resolveCurrencyFormat("RUB", ru);
    CHECK_EQ(formatMoney(123'456, rub), std::string("1234,56 ₽"));

    // Пустой символ -> печатается код валюты вместо знака.
    CurrencyOverride codeOnly;
    codeOnly.symbol = std::string("");
    const CurrencyFormat eurCode = resolveCurrencyFormat("EUR", codeOnly);
    CHECK_EQ(formatMoney(100'000, eurCode), std::string("1 000.00 EUR"));
}
