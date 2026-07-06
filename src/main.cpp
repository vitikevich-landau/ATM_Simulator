// ============================================================================
//  main.cpp — точка входа консольного симулятора банкомата.
//
//  Этап M2: загружаем конфигурацию и прогоняем ОДНОПОТОЧНУЮ (дискретно-событийную)
//  симуляцию очереди, печатаем сводную статистику СМО. Многопоточность и живой
//  дашборд появятся в M3/M6. Ошибки конфигурации (§6.5) ловятся здесь.
// ============================================================================

#include <iomanip>
#include <iostream>
#include <string>

#include "atmsim/Version.hpp"
#include "atmsim/config/ConfigLoader.hpp"
#include "atmsim/engine/SimulationRunner.hpp"

using namespace atmsim;

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/default_config.json";

    try {
        const Config cfg = ConfigLoader::loadFromFile(path);

        std::cout << kProjectName << " v" << kVersion << " — однопоточная симуляция (M2)\n";
        std::cout << "Конфиг: " << path << "  |  seed=" << cfg.simulation.randomSeed << "\n\n";

        const SimulationResult r = SimulationRunner::run(cfg);

        std::cout << std::fixed << std::setprecision(1);
        std::cout << "=== Результаты симуляции ===\n";
        std::cout << "Клиентов всего:      " << r.totalClients << '\n';
        std::cout << "Обслужено:           " << r.served
                  << "  (успех " << r.opSuccess << ", отказ " << r.opFailed << ")\n";
        std::cout << "Ушли по терпению:    " << r.leftByPatience << '\n';
        std::cout << "Среднее ожидание:    " << r.avgWaitSeconds << " c\n";
        std::cout << "Среднее обслуж.:     " << r.avgServiceSeconds << " c\n";
        std::cout << "Макс. длина очереди: " << r.maxQueueLength << '\n';
        std::cout << "Загрузка rho=lambda/mu: " << std::setprecision(2) << r.rhoTheoretical
                  << (r.rhoTheoretical > 1.0 ? "  (>1 — система перегружена)" : "") << '\n';
        std::cout << "Факт. загрузка:      " << r.serverUtilization << '\n';
        std::cout << std::setprecision(1)
                  << "Длительность:        " << r.totalModelSeconds << " c модельного времени\n\n";

        std::cout << "Касса:  " << formatMoney(r.cashStart) << " -> " << formatMoney(r.cashEnd)
                  << ' ' << cfg.atm.currency << '\n';
        std::cout << "Счета:  " << formatMoney(r.accountsStart) << " -> " << formatMoney(r.accountsEnd)
                  << ' ' << cfg.atm.currency << '\n';
        std::cout << "Инвариант денег (счета - касса неизменны): "
                  << (r.moneyConserved ? "OK" : "НАРУШЕН!") << '\n';
    } catch (const ConfigError& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
