#pragma once
// ============================================================================
//  ClientFactory.hpp — генерация клиентов по конфигурации (Factory, §6.4).
// ============================================================================
#include <random>
#include <vector>

#include "atmsim/config/Config.hpp"
#include "atmsim/core/Types.hpp"

namespace atmsim {

// Клиент в терминах дискретно-событийной симуляции (M2): время прихода — это
// число модельных СЕКУНД от старта, а не std::chrono::time_point. Реальный
// runtime-Client (core/Client.hpp) с настенными часами понадобится в M3, где
// симуляция станет многопоточной и «живой». Здесь же удобнее простые секунды.
struct SimClient {
    ClientId id;
    AccountId accountId;
    double arrivalSec;       // когда пришёл (модельные секунды от старта)
    OperationType op;
    Money amount;            // 0 для CheckBalance
    double patienceSec;      // сколько готов ждать в очереди
};

class ClientFactory {
public:
    explicit ClientFactory(const ClientsConfig& cfg) : cfg_(cfg) {}

    // Генерирует список из cfg.count клиентов. Времена прихода:
    //   * poisson — интервалы между приходами экспоненциальные (среднее 60/rate с);
    //   * batch   — приходят равномерно, с постоянным шагом 60/rate с.
    // Все случайные величины берутся из переданного rng — один seed на прогон.
    std::vector<SimClient> generate(std::mt19937_64& rng) const;

private:
    ClientsConfig cfg_;
};

}  // namespace atmsim
