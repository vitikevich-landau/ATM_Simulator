// ============================================================================
//  main.cpp — точка входа консольного симулятора банкомата.
//
//  Этап M2 (часть a): загружаем конфигурацию и печатаем её сводку. Сама
//  симуляция очереди приедет в M2 (часть b). Ошибки конфигурации (§6.5) —
//  это исключения ConfigError, которые ловятся здесь и приводят к аккуратному
//  завершению с сообщением, а не к падению.
// ============================================================================

#include <iostream>
#include <string>

#include "atmsim/Version.hpp"
#include "atmsim/config/ConfigLoader.hpp"

using namespace atmsim;

int main(int argc, char** argv) {
    // Путь к конфигу — из аргумента командной строки, иначе файл по умолчанию.
    const std::string path = (argc > 1) ? argv[1] : "config/default_config.json";

    try {
        const Config cfg = ConfigLoader::loadFromFile(path);

        std::cout << kProjectName << " v" << kVersion << '\n';
        std::cout << "Конфигурация загружена из: " << path << "\n\n";
        std::cout << "  Касса:        " << formatMoney(cfg.atm.initialCash) << ' '
                  << cfg.atm.currency << "  (порог инкассации "
                  << formatMoney(cfg.atm.lowCashThreshold) << ")\n";
        std::cout << "  Приход:       "
                  << (cfg.clients.arrivalMode == ArrivalMode::Poisson ? "poisson" : "batch")
                  << ", " << cfg.clients.arrivalRatePerMinute << "/мин, клиентов: "
                  << cfg.clients.count << '\n';
        std::cout << "  Обслуживание: "
                  << (cfg.serviceTime.distribution == ServiceDistribution::Normal ? "normal"
                      : cfg.serviceTime.distribution == ServiceDistribution::Uniform ? "uniform"
                      : "exponential")
                  << ", среднее " << cfg.serviceTime.meanSeconds << " c\n";
        std::cout << "  Воспроизв.:   seed=" << cfg.simulation.randomSeed
                  << ", time_scale=" << cfg.simulation.timeScale << '\n';
        std::cout << "\nЭтап M2a: конфигурация читается. Симуляция очереди — следующий шаг.\n";
    } catch (const ConfigError& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
