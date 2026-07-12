#pragma once
// ============================================================================
//  Logger.hpp — технический лог уровня DEBUG/INFO/WARN/ERROR (§5, §10).
//
//  Это ВТОРОЙ, независимый поток информации — для разработчика, отдельно от
//  бизнес-журнала операций (который видит администратор через operations/export).
//  Пишет в файл, потокобезопасен (его дёргают поток обслуживания и главный поток).
// ============================================================================
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace atmsim {

/// \brief Уровень важности сообщения технического лога.
enum class LogLevel { Debug, Info, Warn, Error };

/// \brief Потокобезопасный файловый логгер. Сообщения ниже минимального уровня
///        отбрасываются. Каждая строка снабжается меткой времени и уровнем.
class Logger {
public:
    Logger(const std::string& file, LogLevel minLevel);

    bool ok() const { return static_cast<bool>(out_); }

    void log(LogLevel level, const std::string& message);
    void debug(const std::string& m) { log(LogLevel::Debug, m); }
    void info(const std::string& m)  { log(LogLevel::Info, m); }
    void warn(const std::string& m)  { log(LogLevel::Warn, m); }
    void error(const std::string& m) { log(LogLevel::Error, m); }

    /// Переоткрыть лог под НОВЫЙ файл/уровень — для restart с перечитанным
    /// конфигом (logging.file / logging.level могли измениться). В отличие от
    /// конструктора (trunc — свежий лог на запуск процесса), дописывает (append):
    /// логи прошлых прогонов той же сессии не теряются. Потокобезопасно; вызывать
    /// между прогонами, когда потоки движка уже остановлены.
    void reconfigure(const std::string& file, LogLevel minLevel);

    /// Разбирает строку уровня из конфига ("debug"/"info"/"warn"/"warning"/
    /// "error"). Неизвестное значение -> Info (безопасный дефолт; сохранён ради
    /// совместимости API). Чтобы ОТЛИЧИТЬ неизвестное от валидного — tryParseLevel.
    static LogLevel parseLevel(const std::string& s);

    /// Как parseLevel, но неизвестный уровень возвращает std::nullopt (а не молча
    /// Info) — вызывающий может предупредить/отвергнуть. Разбор и печать уровней
    /// читают ОДНУ таблицу (Logger.cpp), поэтому написание и метка не расходятся.
    static std::optional<LogLevel> tryParseLevel(const std::string& s);

private:
    std::ofstream out_;
    LogLevel min_;
    std::mutex mutex_;
};

}  // namespace atmsim
