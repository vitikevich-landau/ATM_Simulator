#include "atmsim/console/AdminConsole.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "atmsim/console/Terminal.hpp"
#include "atmsim/core/Money.hpp"

namespace atmsim {
namespace {

// Форматирует момент времени журнала как HH:MM:SS (локальное время).
std::string formatTime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    const std::tm* lt = std::localtime(&t);
    char buf[16];
    if (lt && std::strftime(buf, sizeof(buf), "%H:%M:%S", lt) > 0) return buf;
    return "??:??:??";
}

void printRecord(const OperationRecord& r) {
    std::cout << "  [" << formatTime(r.timestamp) << "] #" << r.clientId << ' ' << to_string(r.type);
    if (r.type != OperationType::CheckBalance) std::cout << ' ' << formatMoney(r.amount);
    if (r.success) std::cout << " — успех, баланс " << formatMoney(r.balanceAfter);
    else std::cout << " — отказ (" << r.errorMessage << ')';
    std::cout << '\n';
}

}  // namespace

AdminConsole::AdminConsole(AtmEngine& engine, const Config& cfg)
    : engine_(engine), cfg_(cfg) {}

// ---------------------------------------------------------------------------
//  Печать отчётов
// ---------------------------------------------------------------------------
void AdminConsole::printHelp() const {
    std::cout <<
        "Команды:\n"
        "  help                              — эта справка\n"
        "  status                            — общий статус банкомата\n"
        "  queue                             — список ожидающих в очереди\n"
        "  client <id>                       — отчёт по клиенту\n"
        "  balance <id>                      — баланс счёта клиента\n"
        "  operations [--client N] [--last N] [--type T]\n"
        "                                    — журнал операций (T: withdraw/deposit/check)\n"
        "  atm                               — состояние банкомата (касса, аптайм)\n"
        "  stats                             — статистика СМО\n"
        "  pause / resume                    — приостановить / возобновить обслуживание\n"
        "  maintenance start [сек] / stop    — техобслуживание\n"
        "  live / live off                   — включить / выключить живой дашборд\n"
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
    std::cout << "Ушли (всего):      " << s.totalLeft << '\n';
    std::cout << "Касса:             " << formatMoney(s.cashboxBalance) << ' ' << cfg_.atm.currency
              << (s.lowCash ? "  [НИЗКАЯ КАССА]" : "") << '\n';
    if (s.state == AtmState::Maintenance) {
        std::cout << "ТО: ";
        if (s.maintenanceEtaSeconds < 0.0) std::cout << "до команды maintenance stop\n";
        else std::cout << "осталось ~" << static_cast<long>(s.maintenanceEtaSeconds) << " c\n";
    }
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
    std::cout << "Ушли (всего):         " << s.left
              << "  (из них по ТО: " << s.renegedByMaintenance << ")\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Среднее ожидание:     " << s.avgWaitSeconds << " c\n";
    std::cout << "Среднее обслуживание: " << s.avgServiceSeconds << " c\n";
    std::cout << "Макс. длина очереди:  " << s.maxQueueLength << '\n';
    std::cout << std::setprecision(2) << "Загрузка ρ = λ/μ:     " << s.rhoTheoretical << '\n';
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
    out << "op_id,client_id,type,amount_minor,balance_after_minor,success,error\n";
    for (const auto& r : ops) {
        out << r.id << ',' << r.clientId << ',' << to_string(r.type) << ',' << r.amount << ','
            << r.balanceAfter << ',' << (r.success ? "1" : "0") << ',' << r.errorMessage << '\n';
    }
    std::cout << "Экспортировано записей: " << ops.size() << " -> " << filename << '\n';
}

bool AdminConsole::dispatchReport(const Command& c) const {
    switch (c.type) {
        case CommandType::Help:       printHelp(); return true;
        case CommandType::Status:     printStatus(); return true;
        case CommandType::Queue:      printQueue(); return true;
        case CommandType::Client:     printClient(*c.clientId); return true;
        case CommandType::Balance:    printBalance(*c.clientId); return true;
        case CommandType::Operations: printOperations(c); return true;
        case CommandType::Atm:        printAtm(); return true;
        case CommandType::Stats:      printStats(); return true;
        case CommandType::Export:     doExport(c.filename); return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
//  Главный цикл: переключение режимов
// ---------------------------------------------------------------------------
void AdminConsole::run() {
    ttyAnsi_ = Terminal::isStdoutTty();
    if (ttyAnsi_) Terminal::enableAnsi();
    if (cfg_.ui.liveMode && !ttyAnsi_) {
        std::cout << "(stdout не терминал — живой режим отключён, работаю в командном)\n";
    }

    Next next = (cfg_.ui.liveMode && ttyAnsi_) ? Next::Live : Next::Command;
    if (next == Next::Command) printHelp();

    while (next != Next::Quit) {
        next = (next == Next::Live) ? runLiveSession() : runCommandLoop();
    }
}

// ---------------------------------------------------------------------------
//  Командный режим (pull-REPL)
// ---------------------------------------------------------------------------
AdminConsole::Next AdminConsole::runCommandLoop() {
    std::cout << "\n[командный режим] help — команды, live — дашборд, stop — выход\n";
    std::string line;
    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) return Next::Quit;

        const Command c = parseCommand(line);
        if (!c.error.empty()) {
            std::cout << "Ошибка: " << c.error << '\n';
            continue;
        }

        switch (c.type) {
            case CommandType::Empty: break;
            case CommandType::Pause:  engine_.requestPause();  std::cout << "Обслуживание приостановлено.\n"; break;
            case CommandType::Resume: engine_.requestResume(); std::cout << "Обслуживание возобновлено.\n"; break;
            case CommandType::MaintenanceStart: engine_.requestMaintenance(c.seconds); std::cout << "Начато техобслуживание.\n"; break;
            case CommandType::MaintenanceStop:  engine_.endMaintenance(); std::cout << "Техобслуживание завершено.\n"; break;
            case CommandType::Live:
                if (ttyAnsi_) return Next::Live;
                std::cout << "Живой режим недоступен: stdout не терминал.\n";
                break;
            case CommandType::LiveOff: break;  // уже в командном режиме
            case CommandType::Stop:
                std::cout << "Останавливаюсь...\n";
                engine_.requestStop();
                return Next::Quit;
            case CommandType::Unknown:
                std::cout << "Неизвестная команда: '" << c.word << "'. Наберите help.\n";
                break;
            default:
                dispatchReport(c);  // отчёты и справка
                break;
        }
        if (engine_.isStopped()) return Next::Quit;
    }
}

// ---------------------------------------------------------------------------
//  Живой режим (дашборд + ввод внизу)
// ---------------------------------------------------------------------------
void AdminConsole::showOverlay(LiveRenderer& renderer, const std::function<void()>& printFn) {
    // Приостанавливаем перерисовку, показываем ответ на весь экран, ждём Enter.
    renderer.pause();
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::clearScreen() << ansi::home() << std::flush;
    }
    printFn();
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << "\n-- нажмите Enter, чтобы вернуться к дашборду --" << std::flush;
    }
    std::string dummy;
    std::getline(std::cin, dummy);
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::clearScreen() << std::flush;
    }
    renderer.resume();
}

AdminConsole::Next AdminConsole::runLiveSession() {
    LiveRenderer renderer(engine_, cfg_);
    const int inputRow = renderer.height() + 2;  // строка ввода — НИЖЕ дашборда

    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::altScreenOn() << ansi::clearScreen() << std::flush;
    }
    renderer.start();

    Next result = Next::Command;
    std::string line;
    bool exitLoop = false;
    while (!exitLoop) {
        // Рисуем строку ввода внизу. Рендерер её не трогает (пишет только выше),
        // поэтому набираемый текст не затирается очередным кадром (§4.8.5).
        {
            std::lock_guard<std::mutex> lk(renderer.outputMutex());
            std::cout << ansi::moveTo(inputRow, 1) << ansi::clearToLineEnd() << "cmd> " << std::flush;
        }
        if (!std::getline(std::cin, line)) { result = Next::Quit; break; }

        const Command c = parseCommand(line);
        if (!c.error.empty()) {
            showOverlay(renderer, [&] { std::cout << "Ошибка: " << c.error << '\n'; });
            continue;
        }

        switch (c.type) {
            case CommandType::Empty: break;
            case CommandType::Pause:  engine_.requestPause();  break;  // видно на дашборде
            case CommandType::Resume: engine_.requestResume(); break;
            case CommandType::MaintenanceStart: engine_.requestMaintenance(c.seconds); break;
            case CommandType::MaintenanceStop:  engine_.endMaintenance(); break;
            case CommandType::Live: break;  // уже в живом режиме
            case CommandType::LiveOff: result = Next::Command; exitLoop = true; break;
            case CommandType::Stop:
                engine_.requestStop();
                result = Next::Quit;
                exitLoop = true;
                break;
            case CommandType::Unknown:
                showOverlay(renderer, [&] { std::cout << "Неизвестная команда: '" << c.word << "'.\n"; });
                break;
            default:
                // Отчёт/справка — полноэкранным ответом поверх дашборда.
                showOverlay(renderer, [&] { dispatchReport(c); });
                break;
        }
        if (!exitLoop && engine_.isStopped()) { result = Next::Quit; break; }
    }

    renderer.stop();
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::altScreenOff() << ansi::showCursor() << std::flush;
    }
    return result;
}

}  // namespace atmsim
