// ============================================================================
//  main.cpp — точка входа консольного симулятора банкомата.
//
//  Этап M3: симуляция стала МНОГОПОТОЧНОЙ. Здесь мы связываем всё вместе:
//    * поток обслуживания (AtmEngine::run) и поток прихода клиентов —
//      каждый в своём std::jthread;
//    * консоль администратора (AdminConsole) — в главном потоке.
//  std::jthread сам пошлёт request_stop() и сделает join() в деструкторе (RAII),
//  поэтому вручную об этом помнить не нужно (§6.2).
// ============================================================================

#include <iostream>
#include <stop_token>
#include <string>
#include <thread>

#include "atmsim/Version.hpp"
#include "atmsim/config/ConfigLoader.hpp"
#include "atmsim/console/AdminConsole.hpp"
#include "atmsim/engine/AtmEngine.hpp"

using namespace atmsim;

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/default_config.json";

    try {
        const Config cfg = ConfigLoader::loadFromFile(path);

        std::cout << kProjectName << " v" << kVersion << " — многопоточная симуляция (M3)\n";
        std::cout << "Конфиг: " << path << "  |  клиентов: " << cfg.clients.count << "\n\n";

        AtmEngine engine(cfg);

        // Запускаем два рабочих потока. Лямбда получает stop_token от jthread.
        std::jthread engineThread([&engine](std::stop_token st) { engine.run(st); });
        std::jthread arrivalThread([&engine](std::stop_token st) { engine.generateArrivals(st); });

        // Консоль блокирует главный поток до команды stop/exit или конца ввода.
        AdminConsole console(engine, cfg);
        console.run();

        // Гарантируем остановку движка (если вышли по EOF, а не по stop).
        engine.requestStop();
        engineThread.request_stop();
        arrivalThread.request_stop();
        engineThread.join();
        arrivalThread.join();

        // Итоговая сводка (полная статистика — на M4).
        const AtmSnapshot s = engine.snapshot();
        std::cout << "\nИтог: обслужено " << s.totalServed
                  << ", ушли по терпению " << s.totalLeft
                  << ", касса " << formatMoney(s.cashboxBalance) << ' ' << cfg.atm.currency << '\n';
    } catch (const ConfigError& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
