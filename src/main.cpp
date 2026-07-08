// ============================================================================
//  main.cpp — точка входа консольного симулятора банкомата.
//
//  Связывает всё вместе (M7): технический лог в файл, обработка SIGINT/SIGTERM
//  для плавной остановки (§4.6), печать итоговой статистики СМО при завершении.
//  Потоки движка/прихода — обычные std::thread: main явно делает
//  requestStop()+join() (jthread/stop_token несовместимы с рядом версий MSVC —
//  см. AtmEngine.hpp).
// ============================================================================

#include <exception>
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
        // Если файл лога не открылся (нет прав, битый путь) — Logger молча
        // глотает все записи. Раньше это никак не всплывало: пользователь думал,
        // что лог пишется. Предупреждаем в stderr, но не падаем — лог это
        // вспомогательный поток для разработчика, симуляции он не мешает.
        if (!logger.ok()) {
            std::cerr << "Предупреждение: не удалось открыть файл лога '"
                      << cfg.logging.file << "' — техническое логирование отключено\n";
        }
        logger.info("Конфигурация загружена: " + path);

        std::cout << kProjectName << " v" << kVersion << '\n';
        std::cout << "Конфиг: " << path << "  |  клиентов: " << cfg.clients.count
                  << "  |  лог: " << (logger.ok() ? cfg.logging.file : std::string("(отключён)"))
                  << "\n";

        installSignalHandlers();  // SIGINT/SIGTERM -> плавная остановка (§4.6)

        // Цикл прогонов: команда restart возвращает RunOutcome::Restart, и мы
        // пересоздаём движок «с нуля». Каждый перезапуск меняет seed, чтобы прогон
        // получился другим (тот же seed дал бы идентичный результат, §5).
        auto seed = cfg.simulation.randomSeed;
        int runIndex = 1;
        AdminConsole::RunOutcome outcome = AdminConsole::RunOutcome::Restart;
        while (outcome == AdminConsole::RunOutcome::Restart) {
            Config runCfg = cfg;
            runCfg.simulation.randomSeed = seed;

            AtmEngine engine(runCfg, &logger);

            // Обычные std::thread (не jthread): останавливаем явно через
            // engine.requestStop() перед join(). Причина отказа от jthread/stop_token
            // — несовместимость со stop_token на ряде версий MSVC (см. AtmEngine.hpp).
            std::thread engineThread([&engine] { engine.run(); });
            std::thread arrivalThread([&engine] { engine.generateArrivals(); });

            AdminConsole console(engine, runCfg);
            outcome = console.run();  // блокирует до stop/EOF/сигнала/restart

            if (shutdownRequested()) {
                logger.info("Получен сигнал остановки (SIGINT/SIGTERM)");
                std::cout << "\n(получен сигнал остановки — завершаюсь плавно)\n";
                outcome = AdminConsole::RunOutcome::Quit;  // сигнал важнее restart
            }

            engine.requestStop();  // переводит потоки в Stopped и будит их
            engineThread.join();
            arrivalThread.join();

            // --- Итог прогона (§4.4, §10) ---
            const StatsSnapshot st = engine.statsSnapshot();
            logger.info("Итог прогона #" + std::to_string(runIndex) + ": обслужено " +
                        std::to_string(st.served) + ", ушли " + std::to_string(st.left));

            if (outcome == AdminConsole::RunOutcome::Restart) {
                // Краткая строка-разделитель; полный блок печатаем только при выходе.
                std::cout << "\n── Прогон #" << runIndex << " завершён (обслужено "
                          << st.served << ", ушли " << st.left << "). Перезапуск… ──\n";
                ++runIndex;
                ++seed;
                continue;
            }

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
            std::cout << "Касса:                " << formatMoney(s.cashboxBalance) << ' '
                      << cfg.atm.currency << '\n';
        }
    } catch (const ConfigError& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        // Любое иное исключение (напр. std::bad_alloc/std::length_error из
        // reserve в конструкторе движка при экстремальном конфиге, ошибки ФС) —
        // печатаем и выходим с ненулевым кодом, а не роняем процесс в
        // std::terminate. Конструктор движка (и возможный бросок) отрабатывает до
        // создания потоков, поэтому раскрутка стека здесь безопасна.
        std::cerr << "Непредвиденная ошибка: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Непредвиденная ошибка неизвестного типа\n";
        return 1;
    }
    return 0;
}
