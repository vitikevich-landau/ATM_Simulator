#include "atmsim/console/CommandParser.hpp"

#include <limits>
#include <sstream>
#include <vector>

namespace atmsim {
namespace {

// Разбирает строку как ПОЛОЖИТЕЛЬНОЕ целое (номер клиента / N). Возвращает false
// при мусоре или неположительном значении.
bool parsePositive(const std::string& s, long long& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        const long long v = std::stoll(s, &pos);
        if (pos != s.size() || v <= 0) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// Строка -> тип операции (для --type). Принимаем несколько написаний.
std::optional<OperationType> parseOpType(const std::string& s) {
    if (s == "withdraw") return OperationType::Withdraw;
    if (s == "deposit") return OperationType::Deposit;
    if (s == "check" || s == "checkbalance" || s == "check_balance") return OperationType::CheckBalance;
    return std::nullopt;
}

}  // namespace

Command parseCommand(const std::string& line) {
    // Разбиваем строку на слова по пробелам.
    std::istringstream iss(line);
    std::vector<std::string> tok;
    for (std::string t; iss >> t;) tok.push_back(t);

    Command c;
    if (tok.empty()) {
        c.type = CommandType::Empty;
        return c;
    }
    const std::string& cmd = tok[0];
    c.word = cmd;

    if (cmd == "help") {
        c.type = CommandType::Help;
    } else if (cmd == "status") {
        c.type = CommandType::Status;
    } else if (cmd == "queue") {
        c.type = CommandType::Queue;
    } else if (cmd == "atm") {
        c.type = CommandType::Atm;
    } else if (cmd == "stats") {
        c.type = CommandType::Stats;
    } else if (cmd == "pause") {
        c.type = CommandType::Pause;
    } else if (cmd == "resume") {
        c.type = CommandType::Resume;
    } else if (cmd == "live") {
        // "live" — войти в дашборд; "live off" — выйти в командный режим.
        c.type = (tok.size() >= 2 && tok[1] == "off") ? CommandType::LiveOff : CommandType::Live;
    } else if (cmd == "scene") {
        // "scene on|off" — анимированная сцена; без аргумента — переключить.
        c.type = CommandType::Scene;
        if (tok.size() >= 2) {
            if (tok[1] == "on") c.onOff = true;
            else if (tok[1] == "off") c.onOff = false;
            else {
                c.error = "нужно: scene [on|off]";
                return c;
            }
        }
    } else if (cmd == "maintenance") {
        if (tok.size() < 2) {
            c.error = "нужно: maintenance start [сек] | maintenance stop";
            return c;
        }
        if (tok[1] == "start") {
            c.type = CommandType::MaintenanceStart;
            if (tok.size() >= 3) {  // необязательная длительность
                long long n = 0;
                if (!parsePositive(tok[2], n)) {
                    c.error = "неверная длительность ТО: '" + tok[2] + "'";
                    return c;
                }
                // c.seconds — int; без верхней границы сужение static_cast<int>
                // большого long long дало бы <= 0, а движок трактует <= 0 как
                // БЕССРОЧНОЕ ТО (до команды stop). Отсекаем заранее.
                if (n > std::numeric_limits<int>::max()) {
                    c.error = "слишком большая длительность ТО: '" + tok[2] + "'";
                    return c;
                }
                c.seconds = static_cast<int>(n);
            }
        } else if (tok[1] == "stop") {
            c.type = CommandType::MaintenanceStop;
        } else {
            c.error = "ожидалось 'start' или 'stop' после maintenance";
            return c;
        }
    } else if (cmd == "stop" || cmd == "exit" || cmd == "quit") {
        c.type = CommandType::Stop;
    } else if (cmd == "restart" || cmd == "again") {
        c.type = CommandType::Restart;
    } else if (cmd == "client" || cmd == "balance") {
        c.type = (cmd == "client") ? CommandType::Client : CommandType::Balance;
        if (tok.size() < 2) {
            c.error = "нужен номер клиента: " + cmd + " <id>";
            return c;
        }
        long long id = 0;
        if (!parsePositive(tok[1], id)) {
            c.error = "неверный номер клиента: '" + tok[1] + "'";
            return c;
        }
        c.clientId = static_cast<ClientId>(id);
    } else if (cmd == "export") {
        c.type = CommandType::Export;
        if (tok.size() < 2) {
            c.error = "нужно имя файла: export <file>";
            return c;
        }
        c.filename = tok[1];
    } else if (cmd == "operations") {
        c.type = CommandType::Operations;
        // Разбираем опциональные флаги.
        for (std::size_t i = 1; i < tok.size(); ++i) {
            if (tok[i] == "--client") {
                if (i + 1 >= tok.size()) { c.error = "--client требует номер"; return c; }
                long long id = 0;
                if (!parsePositive(tok[++i], id)) { c.error = "неверный --client: '" + tok[i] + "'"; return c; }
                c.clientId = static_cast<ClientId>(id);
            } else if (tok[i] == "--last") {
                if (i + 1 >= tok.size()) { c.error = "--last требует число"; return c; }
                long long n = 0;
                if (!parsePositive(tok[++i], n)) { c.error = "неверный --last: '" + tok[i] + "'"; return c; }
                c.last = static_cast<std::size_t>(n);
            } else if (tok[i] == "--type") {
                if (i + 1 >= tok.size()) { c.error = "--type требует тип операции"; return c; }
                const auto ot = parseOpType(tok[++i]);
                if (!ot) { c.error = "неизвестный тип операции: '" + tok[i] + "'"; return c; }
                c.opType = ot;
            } else {
                c.error = "неизвестный аргумент: '" + tok[i] + "'";
                return c;
            }
        }
    } else {
        c.type = CommandType::Unknown;
    }
    return c;
}

}  // namespace atmsim
