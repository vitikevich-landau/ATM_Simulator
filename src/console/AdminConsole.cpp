#include "atmsim/console/AdminConsole.hpp"

#include <iostream>
#include <sstream>
#include <string>

#include "atmsim/core/Money.hpp"

namespace atmsim {

AdminConsole::AdminConsole(AtmEngine& engine, const Config& cfg)
    : engine_(engine), cfg_(cfg) {}

void AdminConsole::printHelp() const {
    std::cout <<
        "Команды:\n"
        "  help            — эта справка\n"
        "  status          — состояние банкомата (текущий клиент, очередь, касса)\n"
        "  queue           — список ожидающих в очереди\n"
        "  pause / resume  — приостановить / возобновить обслуживание\n"
        "  stop            — плавно остановить и выйти\n";
}

void AdminConsole::printStatus() const {
    // Один снимок под кратким локом — дальше работаем с копией.
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
    std::cout << "Касса:             " << formatMoney(s.cashboxBalance)
              << ' ' << cfg_.atm.currency << '\n';
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
        if (c.requestedOperation != OperationType::CheckBalance) {
            std::cout << ' ' << formatMoney(c.amount);
        }
        std::cout << "  ждёт " << static_cast<long>(c.waitedSeconds)
                  << " c, терпение осталось " << static_cast<long>(c.remainingPatience) << " c\n";
    }
}

void AdminConsole::run() {
    printHelp();
    std::string line;
    while (true) {
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) break;  // EOF (например, конец пайпа)

        // Разбираем первое слово как команду (полноценный парсер с опциями — M4).
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;

        if (cmd == "help") {
            printHelp();
        } else if (cmd == "status") {
            printStatus();
        } else if (cmd == "queue") {
            printQueue();
        } else if (cmd == "pause") {
            engine_.requestPause();
            std::cout << "Обслуживание приостановлено.\n";
        } else if (cmd == "resume") {
            engine_.requestResume();
            std::cout << "Обслуживание возобновлено.\n";
        } else if (cmd == "stop" || cmd == "exit" || cmd == "quit") {
            std::cout << "Останавливаюсь...\n";
            engine_.requestStop();
            break;
        } else {
            std::cout << "Неизвестная команда: '" << cmd << "'. Наберите help.\n";
        }

        // Если движок остановился по другой причине — выходим из цикла.
        if (engine_.isStopped()) break;
    }
}

}  // namespace atmsim
