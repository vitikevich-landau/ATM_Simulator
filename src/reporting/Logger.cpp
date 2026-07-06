#include "atmsim/reporting/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace atmsim {
namespace {

const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
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

void Logger::log(LogLevel level, const std::string& message) {
    // Отсекаем всё ниже минимального уровня (Debug < Info < Warn < Error).
    if (static_cast<int>(level) < static_cast<int>(min_)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_) return;
    out_ << '[' << timestamp() << "] [" << levelName(level) << "] " << message << '\n';
    out_.flush();  // не теряем логи при аварийном завершении
}

LogLevel Logger::parseLevel(const std::string& s) {
    if (s == "debug") return LogLevel::Debug;
    if (s == "info")  return LogLevel::Info;
    if (s == "warn" || s == "warning") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;
}

}  // namespace atmsim
