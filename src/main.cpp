// ============================================================================
//  main.cpp — точка входа консольного симулятора банкомата.
//
//  Связывает всё вместе (M7): технический лог в файл, обработка SIGINT/SIGTERM
//  для плавной остановки (§4.6), печать итоговой статистики СМО при завершении.
//  std::jthread сам делает request_stop()+join() в деструкторе (RAII, §6.2).
// ============================================================================

#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "atmsim/Version.hpp"
#include "atmsim/config/ConfigLoader.hpp"
#include "atmsim/console/AdminConsole.hpp"
#include "atmsim/console/Signals.hpp"
#include "atmsim/engine/AtmEngine.hpp"
#include "atmsim/reporting/Logger.hpp"
#include "utils/ru_console.hpp"

using namespace atmsim;

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/default_config.json";

    try {
        const Config cfg = ConfigLoader::loadFromFile(path);

        // Технический лог (§10) — отдельно от бизнес-журнала операций.
        Logger logger(cfg.logging.file, Logger::parseLevel(cfg.logging.level));
        logger.info("Конфигурация загружена: " + path);

        std::cout << kProjectName << " v" << kVersion << '\n';
        std::cout << "Конфиг: " << path << "  |  клиентов: " << cfg.clients.count
                  << "  |  лог: " << cfg.logging.file << "\n";

        AtmEngine engine(cfg, &logger);
        installSignalHandlers();  // SIGINT/SIGTERM -> плавная остановка (§4.6)

        // Обычные std::thread (не jthread): останавливаем явно через
        // engine.requestStop() перед join(). Причина отказа от jthread/stop_token —
        // несовместимость связки со stop_token на ряде версий MSVC (см. AtmEngine.hpp).
        std::thread engineThread([&engine] { engine.run(); });
        std::thread arrivalThread([&engine] { engine.generateArrivals(); });

        AdminConsole console(engine, cfg);
        console.run();  // блокирует до stop/EOF/сигнала

        if (shutdownRequested()) {
            logger.info("Получен сигнал остановки (SIGINT/SIGTERM)");
            std::cout << "\n(получен сигнал остановки — завершаюсь плавно)\n";
        }

        engine.requestStop();  // переводит потоки в Stopped и будит их
        engineThread.join();
        arrivalThread.join();

        // --- Итоговая статистика СМО (§4.4, §10) ---
        const StatsSnapshot st = engine.statsSnapshot();
        const AtmSnapshot s = engine.snapshot();
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "\n=== Итоговая статистика ===\n";
        std::cout << "Обслужено:            " << st.served << '\n';
        std::cout << "Ушли (всего):         " << st.left
                  << "  (из них по ТО: " << st.renegedByMaintenance << ")\n";
        std::cout << "Среднее ожидание:     " << st.avgWaitSeconds << " c\n";
        std::cout << "Среднее обслуживание: " << st.avgServiceSeconds << " c\n";
        std::cout << "Макс. длина очереди:  " << st.maxQueueLength << '\n';
        std::cout << std::setprecision(2) << "Загрузка ρ = λ/μ:     " << st.rhoTheoretical << '\n';
        std::cout << "Факт. загрузка:       " << st.utilization << '\n';
        std::cout << std::setprecision(0) << "Аптайм:               " << st.uptimeSeconds << " c\n";
        std::cout.unsetf(std::ios::floatfield);
        std::cout << "Касса:                " << formatMoney(s.cashboxBalance) << ' ' << cfg.atm.currency << '\n';

        logger.info("Итог: обслужено " + std::to_string(st.served) +
                    ", ушли " + std::to_string(st.left));
    } catch (const ConfigError& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
