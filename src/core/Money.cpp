#include "atmsim/core/Money.hpp"

#include <string_view>

namespace atmsim {

std::string formatMoney(Money minor) {
    // Знак обрабатываем отдельно, а с величиной работаем как с беззнаковой:
    // так дробная часть не получится отрицательной (−5 центов -> "-0.05", а не
    // "-0.-5"). Перевод в uint64 корректно даёт модуль даже для INT64_MIN.
    const bool negative = minor < 0;
    const std::uint64_t abs =
        negative ? (~static_cast<std::uint64_t>(minor) + 1u)   // = -minor
                 : static_cast<std::uint64_t>(minor);

    const std::uint64_t unit  = static_cast<std::uint64_t>(kMinorUnitsPerMajor);
    const std::uint64_t major = abs / unit;   // целая часть (например, 500)
    const std::uint64_t frac  = abs % unit;   // дробная часть (например, 05)

    std::string s;
    if (negative) s += '-';
    s += std::to_string(major);
    s += '.';
    // Дробная часть — ровно два знака, т.к. kMinorUnitsPerMajor == 100.
    // (Если валюта поменяет число знаков, эту функцию нужно обобщить.)
    if (frac < 10) s += '0';   // ведущий ноль: 5 -> "05"
    s += std::to_string(frac);
    return s;
}

namespace {

// Группирует целую часть числа: вставляет sep каждые 3 цифры, считая справа.
// sep пуст -> без группировки. digits — только цифры (без знака и точки).
//   "500000", " " -> "500 000";  "1234", "," -> "1,234";  "42", " " -> "42".
std::string groupDigits(const std::string& digits, const std::string& sep) {
    if (sep.empty() || digits.size() <= 3) return digits;
    // Первая группа — «остаток» (1..3 цифры), дальше строго по 3.
    const std::size_t head = (digits.size() % 3 == 0) ? 3 : (digits.size() % 3);
    std::string out;
    out.reserve(digits.size() + (digits.size() / 3) * sep.size());
    out.append(digits, 0, head);
    for (std::size_t i = head; i < digits.size(); i += 3) {
        out += sep;
        out.append(digits, i, 3);
    }
    return out;
}

}  // namespace

std::string formatMoney(Money minor, const CurrencyFormat& fmt, bool withCurrency) {
    // Знак и модуль — как в базовом formatMoney: перевод в uint64 даёт модуль даже
    // для INT64_MIN без UB.
    const bool negative = minor < 0;
    const std::uint64_t abs =
        negative ? (~static_cast<std::uint64_t>(minor) + 1u) : static_cast<std::uint64_t>(minor);
    const std::uint64_t unit  = static_cast<std::uint64_t>(kMinorUnitsPerMajor);
    const std::uint64_t major = abs / unit;
    const std::uint64_t frac  = abs % unit;   // 0..99 (kMinorUnitsPerMajor == 100)

    // Целая часть с группировкой разрядов + дробная часть нужной длины.
    std::string number = groupDigits(std::to_string(major), fmt.groupSep);
    if (fmt.decimals > 0) {
        // Дробь всегда двузначная (сотые); приводим к fmt.decimals: 2 — как есть,
        // >2 — дополняем нулями справа, 1 — оставляем старший знак (усечение).
        std::string fracStr;
        fracStr += static_cast<char>('0' + frac / 10);
        fracStr += static_cast<char>('0' + frac % 10);
        const std::size_t want = static_cast<std::size_t>(fmt.decimals);
        if (want < fracStr.size()) fracStr.resize(want);
        else if (want > fracStr.size()) fracStr.append(want - fracStr.size(), '0');
        number += fmt.decimalSep;
        number += fracStr;
    }

    const std::string& token = fmt.symbol.empty() ? fmt.code : fmt.symbol;
    std::string out;
    if (negative) out += '-';
    if (!withCurrency || token.empty()) {
        out += number;
        return out;
    }
    const char* gap = fmt.spaceBetween ? " " : "";
    if (fmt.symbolBefore) { out += token; out += gap; out += number; }
    else                  { out += number; out += gap; out += token; }
    return out;
}

CurrencyFormat builtinCurrencyFormat(std::string_view code) {
    // Десятичный знак по умолчанию '.' единообразно с проектом; специфичны символ,
    // его позиция и разделитель разрядов. Неизвестный код -> код как токен,
    // группировка пробелом (безопасный, читаемый дефолт).
    CurrencyFormat f;
    f.code = std::string(code);
    f.decimalSep = ".";
    f.decimals = 2;
    if (code == "EUR")      { f.symbol = "€"; f.symbolBefore = false; f.spaceBetween = true;  f.groupSep = " "; }
    else if (code == "USD") { f.symbol = "$"; f.symbolBefore = true;  f.spaceBetween = false; f.groupSep = ","; }
    else if (code == "GBP") { f.symbol = "£"; f.symbolBefore = true;  f.spaceBetween = false; f.groupSep = ","; }
    else if (code == "RUB") { f.symbol = "₽"; f.symbolBefore = false; f.spaceBetween = true;  f.groupSep = " "; }
    else if (code == "JPY") { f.symbol = "¥"; f.symbolBefore = true;  f.spaceBetween = false; f.groupSep = ","; f.decimals = 0; }
    else                    { f.symbol = "";  f.symbolBefore = false; f.spaceBetween = true;  f.groupSep = " "; }  // код как токен
    return f;
}

CurrencyFormat resolveCurrencyFormat(std::string_view code, const CurrencyOverride& ov) {
    CurrencyFormat f = builtinCurrencyFormat(code);
    if (ov.symbol)           f.symbol = *ov.symbol;             // "" -> печатать код (fmt.code)
    if (ov.symbolBefore)     f.symbolBefore = *ov.symbolBefore;
    if (ov.spaceBetween)     f.spaceBetween = *ov.spaceBetween;
    if (ov.groupSeparator)   f.groupSep = *ov.groupSeparator;
    if (ov.decimalSeparator) f.decimalSep = *ov.decimalSeparator;
    if (ov.decimals)         f.decimals = *ov.decimals;
    return f;
}

}  // namespace atmsim
