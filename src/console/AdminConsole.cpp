#include "atmsim/console/AdminConsole.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "atmsim/core/Money.hpp"

namespace atmsim {
namespace {

// Форматирует момент времени журнала как HH:MM:SS (локальное время).
// std::localtime не потокобезопасен, но консоль работает в одном потоке.
std::string formatTime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    const std::tm* lt = std::localtime(&t);
    char buf[16];
    if (lt && std::strftime(buf, sizeof(buf), "%H:%M:%S", lt) > 0) return buf;
    return "??:??:??";
}

// Печатает одну строку журнала операций.
void printRecord(const OperationRecord& r) {
    std::cout << "  [" << formatTime(r.timestamp) << "] #" << r.clientId << ' '
              << to_string(r.type);
    if (r.type != OperationType::CheckBalance) std::cout << ' ' << formatMoney(r.amount);
    if (r.success) {
        std::cout << " — успех, баланс " << formatMoney(r.balanceAfter);
    } else {
        std::cout << " — отказ (" << r.errorMessage << ')';
    }
    std::cout << '\n';
}

}  // namespace

AdminConsole::AdminConsole(AtmEngine& engine, const Config& cfg)
    : engine_(engine), cfg_(cfg) {}

void AdminConsole::printHelp() const {
    std::cout <<
        "Команды:\n"
        "  help                              — эта справка\n"
        "  status                            — общий статус банкомата\n"
        "  queue                             — список ожидающих в очереди\n"
        "  client <id>                       — отчёт по клиенту (статус, операции, баланс)\n"
        "  balance <id>                      — баланс счёта клиента\n"
        "  operations [--client N] [--last N] [--type T]\n"
        "                                    — журнал операций с фильтрами (T: withdraw/deposit/check)\n"
        "  atm                               — состояние банкомата (касса, аптайм)\n"
        "  stats                             — статистика СМО (ожидание, загрузка, ρ)\n"
        "  pause / resume                    — приостановить / возобновить обслуживание\n"
        "  export <file>                     — выгрузить журнал операций в CSV\n"
        "  stop                              — плавно остановить и выйти\n";
}

void AdminConsole::printStatus() const {
    const AtmSnapshot s = engine_.snapshot();
    std::cout << "=== Статус банкомата ===\n";
    std::cout << "Состояние:        " << to_string(s.state) << '\n';
    if (s.currentClientId) {
        std::cout << "Текущий клиент:   #" << *s.currentClientId
                  << " (" << to_string(*s.currentOperation) << ")\n";
    } else {
        std::cout << "Текущий клиент:   —\n";
    }
    std::cout << "В очереди:         " << s.queueLength
              << "  (макс. за прогон " << s.maxQueueLength << ")\n";
    std::cout << "Обслужено:         " << s.totalServed << '\n';
    std::cout << "Ушли по терпению:  " << s.totalLeft << '\n';
    std::cout << "Касса:             " << formatMoney(s.cashboxBalance) << ' ' << cfg_.atm.currency
              << (s.lowCash ? "  [НИЗКАЯ КАССА]" : "") << '\n';
}

void AdminConsole::printQueue() const {
    const std::vector<ClientSnapshot> q = engine_.queueSnapshot();
    if (q.empty()) {
        std::cout << "Очередь пуста.\n";
        return;
    }
    std::cout << "В очереди " << q.size() << ":\n";
    for (std::size_t i = 0; i < q.size(); ++i) {
        const ClientSnapshot& c = q[i];
        std::cout << "  " << (i + 1) << ". #" << c.id << ' ' << to_string(c.requestedOperation);
        if (c.requestedOperation != OperationType::CheckBalance) std::cout << ' ' << formatMoney(c.amount);
        std::cout << "  ждёт " << static_cast<long>(c.waitedSeconds)
                  << " c, терпение осталось " << static_cast<long>(c.remainingPatience) << " c\n";
    }
}

void AdminConsole::printClient(ClientId id) const {
    const auto rep = engine_.clientReport(id);
    if (!rep) {
        std::cout << "Клиент #" << id << " не найден.\n";
        return;
    }
    std::cout << "=== Клиент #" << rep->id << " ===\n";
    std::cout << "Статус:      " << to_string(rep->state) << '\n';
    std::cout << "Операция:    " << to_string(rep->requestedOperation);
    if (rep->requestedOperation != OperationType::CheckBalance) std::cout << ' ' << formatMoney(rep->amount);
    std::cout << '\n';
    std::cout << "Терпение:    " << rep->patienceSeconds << " c\n";
    std::cout << "Баланс:      " << formatMoney(rep->accountBalance) << ' ' << cfg_.atm.currency << '\n';
    if (rep->history.empty()) {
        std::cout << "История операций: пусто\n";
    } else {
        std::cout << "История операций:\n";
        for (const auto& r : rep->history) printRecord(r);
    }
}

void AdminConsole::printBalance(ClientId id) const {
    const auto bal = engine_.balanceOf(id);
    if (!bal) {
        std::cout << "Клиент #" << id << " не найден.\n";
        return;
    }
    std::cout << "Баланс клиента #" << id << ": " << formatMoney(*bal) << ' ' << cfg_.atm.currency << '\n';
}

void AdminConsole::printOperations(const Command& c) const {
    OperationFilter f;
    f.client = c.clientId;
    f.type = c.opType;
    f.last = c.last;
    const std::vector<OperationRecord> ops = engine_.operations(f);
    if (ops.empty()) {
        std::cout << "Записей журнала не найдено.\n";
        return;
    }
    std::cout << "Операции (" << ops.size() << "):\n";
    for (const auto& r : ops) printRecord(r);
}

void AdminConsole::printAtm() const {
    const AtmSnapshot s = engine_.snapshot();
    std::cout << "=== Банкомат ===\n";
    std::cout << "Состояние:  " << to_string(s.state) << '\n';
    std::cout << "Касса:      " << formatMoney(s.cashboxBalance) << ' ' << cfg_.atm.currency
              << (s.lowCash ? "  [НИЗКАЯ КАССА]" : "") << '\n';
    std::cout << std::fixed << std::setprecision(0)
              << "Аптайм:     " << s.uptimeSeconds << " c модельного времени\n";
    std::cout.unsetf(std::ios::floatfield);
}

void AdminConsole::printStats() const {
    const StatsSnapshot s = engine_.statsSnapshot();
    std::cout << "=== Статистика СМО ===\n";
    std::cout << "Обслужено:            " << s.served << '\n';
    std::cout << "Ушли по терпению:     " << s.left << '\n';
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Среднее ожидание:     " << s.avgWaitSeconds << " c\n";
    std::cout << "Среднее обслуживание: " << s.avgServiceSeconds << " c\n";
    std::cout << "Макс. длина очереди:  " << s.maxQueueLength << '\n';
    std::cout << std::setprecision(2)
              << "Загрузка ρ = λ/μ:     " << s.rhoTheoretical << '\n';
    std::cout << "Факт. загрузка:       " << s.utilization << '\n';
    std::cout << std::setprecision(0) << "Аптайм:               " << s.uptimeSeconds << " c\n";
    std::cout.unsetf(std::ios::floatfield);
}

void AdminConsole::doExport(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary);
    if (!out) {
        std::cout << "Не удалось открыть файл для записи: " << filename << '\n';
        return;
    }
    const std::vector<OperationRecord> ops = engine_.operations(OperationFilter{});
    // CSV-заголовок и строки. Суммы — в минорных единицах (как хранятся).
    out << "op_id,client_id,type,amount_minor,balance_after_minor,success,error\n";
    for (const auto& r : ops) {
        out << r.id << ',' << r.clientId << ',' << to_string(r.type) << ',' << r.amount << ','
            << r.balanceAfter << ',' << (r.success ? "1" : "0") << ',' << r.errorMessage << '\n';
    }
    std::cout << "Экспортировано записей: " << ops.size() << " -> " << filename << '\n';
}

void AdminConsole::run() {
    printHelp();
    std::string line;
    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) break;  // EOF (например, конец пайпа)

        const Command c = parseCommand(line);
        if (!c.error.empty()) {
            std::cout << "Ошибка: " << c.error << '\n';
            continue;
        }

        switch (c.type) {
            case CommandType::Empty: break;
            case CommandType::Help: printHelp(); break;
            case CommandType::Status: printStatus(); break;
            case CommandType::Queue: printQueue(); break;
            case CommandType::Client: printClient(*c.clientId); break;
            case CommandType::Balance: printBalance(*c.clientId); break;
            case CommandType::Operations: printOperations(c); break;
            case CommandType::Atm: printAtm(); break;
            case CommandType::Stats: printStats(); break;
            case CommandType::Pause:
                engine_.requestPause();
                std::cout << "Обслуживание приостановлено.\n";
                break;
            case CommandType::Resume:
                engine_.requestResume();
                std::cout << "Обслуживание возобновлено.\n";
                break;
            case CommandType::Export: doExport(c.filename); break;
            case CommandType::Stop:
                std::cout << "Останавливаюсь...\n";
                engine_.requestStop();
                return;
            case CommandType::Unknown:
                std::cout << "Неизвестная команда: '" << c.word << "'. Наберите help.\n";
                break;
        }

        if (engine_.isStopped()) break;
    }
}

}  // namespace atmsim
