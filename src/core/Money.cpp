#include "atmsim/core/Money.hpp"

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

}  // namespace atmsim
