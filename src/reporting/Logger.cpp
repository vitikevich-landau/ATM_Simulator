#include "atmsim/reporting/Logger.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace atmsim {
namespace {

// Единая таблица уровней: и разбор строки (вход), и печать метки (выход) читают
// ЕЁ ЖЕ — поэтому написание токена и отображаемая метка не могут разъехаться, а
// добавление уровня — одна строка. canonical=false — это ВХОДНОЙ алиас
// ("warning" тоже принимаем на входе), для печати используется только
// canonical-строка. Раньше разбор (parseLevel) и печать (levelName) были двумя
// независимыми switch'ами: parseLevel принимал "warning", а levelName про него
// не знал; вход был в нижнем регистре, печать — в верхнем, и синхронизировать их
// приходилось вручную.
struct LevelRow {
    std::string_view token;    // как пишется в конфиге (нижний регистр)
    LogLevel level;
    std::string_view display;  // как печатается в логе (верхний регистр)
    bool canonical;            // true — основное написание (для печати)
};
inline constexpr std::array<LevelRow, 5> kLevels{{
    {"debug",   LogLevel::Debug, "DEBUG", true},
    {"info",    LogLevel::Info,  "INFO",  true},
    {"warn",    LogLevel::Warn,  "WARN",  true},
    {"warning", LogLevel::Warn,  "WARN",  false},  // входной алиас "warning"
    {"error",   LogLevel::Error, "ERROR", true},
}};

std::string_view levelName(LogLevel l) {
    for (const LevelRow& r : kLevels) {
        if (r.level == l && r.canonical) return r.display;
    }
    // Недостижимо: у каждого LogLevel есть canonical-строка в kLevels.
    assert(false && "нет canonical-метки для LogLevel");
    return "?";
}

// Метка времени [HH:MM:SS.mmm]. localtime не потокобезопасен, но вызывается
// только под mutex_ логгера, поэтому здесь это безопасно.
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    char buf[16];
    const std::tm* lt = std::localtime(&t);
    if (!lt || std::strftime(buf, sizeof(buf), "%H:%M:%S", lt) == 0) return "??:??:??";
    std::ostringstream os;
    os << buf << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return os.str();
}

}  // namespace

Logger::Logger(const std::string& file, LogLevel minLevel)
    : out_(file, std::ios::binary | std::ios::trunc), min_(minLevel) {}

void Logger::reconfigure(const std::string& file, LogLevel minLevel) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_ = minLevel;
    out_.close();
    out_.clear();  // сброс флагов ошибки, иначе повторный open может не сработать
    // append (а не trunc): при restart в тот же файл логи прошлых прогонов сессии
    // сохраняются; при смене logging.file — открываем/дописываем новый файл.
    out_.open(file, std::ios::binary | std::ios::app);
}

void Logger::log(LogLevel level, const std::string& message) {
    // Отсекаем всё ниже минимального уровня (Debug < Info < Warn < Error).
    if (static_cast<int>(level) < static_cast<int>(min_)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_) return;
    out_ << '[' << timestamp() << "] [" << levelName(level) << "] " << message << '\n';
    out_.flush();  // не теряем логи при аварийном завершении
}

std::optional<LogLevel> Logger::tryParseLevel(const std::string& s) {
    for (const LevelRow& r : kLevels) {
        if (r.token == s) return r.level;
    }
    return std::nullopt;  // неизвестный уровень — пусть решает вызывающий
}

LogLevel Logger::parseLevel(const std::string& s) {
    // Лёгкий разбор с безопасным дефолтом (сохранён ради существующего API и
    // теста): неизвестное значение -> Info. Кто хочет ОТЛИЧИТЬ неизвестное от
    // валидного (main предупреждает в stderr) — использует tryParseLevel.
    return tryParseLevel(s).value_or(LogLevel::Info);
}

}  // namespace atmsim
