#pragma once
// ============================================================================
//  Config.hpp — параметры симуляции (соответствуют config/default_config.json).
//
//  Это простая структура-«пакет настроек» со значениями по умолчанию. Она НЕ
//  знает про JSON — загрузку из файла делает ConfigLoader (там и только там
//  подключается nlohmann/json). Так остальной код не зависит от формата файла.
// ============================================================================
#include <cstdint>
#include <string>

#include "atmsim/core/Money.hpp"

namespace atmsim {

// Режим прихода клиентов (§4.1).
enum class ArrivalMode { Batch, Poisson };

// Распределение времени обслуживания (§6.3).
enum class ServiceDistribution { Uniform, Normal, Exponential };

struct AtmConfig {
    Money initialCash = 50'000'000;      // 500 000.00
    Money lowCashThreshold = 2'000'000;  //  20 000.00 — порог инкассации (§4.8)
    std::string currency = "EUR";
};

// Веса вероятностей операций. Не обязаны в сумме давать 1 — нормализуются при
// выборе (M2b). Требование: неотрицательны и в сумме > 0.
struct OperationWeights {
    double checkBalance = 0.3;
    double withdraw = 0.5;
    double deposit = 0.2;
};

struct AmountRange {  // диапазон сумм операции, в минорных единицах
    Money min = 50'000;      // 500.00
    Money max = 3'000'000;   // 30 000.00
};

struct SecondsRange {  // диапазон терпения клиента, в секундах
    int min = 30;
    int max = 300;
};

struct ClientsConfig {
    ArrivalMode arrivalMode = ArrivalMode::Poisson;
    double arrivalRatePerMinute = 4.0;   // для poisson: среднее число в минуту
    OperationWeights weights;
    AmountRange amountRange;
    SecondsRange patienceSeconds;

    // Эти два поля добавлены для однопоточной симуляции (M2): сколько всего
    // клиентов сгенерировать за прогон и с каким балансом заводить их счета.
    int count = 50;
    Money initialBalance = 100'000;  // 1000.00
};

struct ServiceTimeConfig {
    ServiceDistribution distribution = ServiceDistribution::Normal;
    double meanSeconds = 25.0;    // для normal/exponential
    double stddevSeconds = 7.0;   // для normal
    double minSeconds = 5.0;      // нижняя отсечка (время не бывает < 0)
    double maxSeconds = 60.0;     // верхняя граница для uniform
};

struct MaintenanceConfig {
    double renegeProbability = 0.6;      // вероятность ухода при ТО (§4.5)
    int defaultDurationSeconds = 120;
    bool arrivalsContinue = true;        // приходят ли клиенты во время ТО
};

struct SimulationConfig {
    std::uint64_t randomSeed = 42;  // фиксированный seed => воспроизводимость (§5)
    double timeScale = 1.0;         // ускорение модельного времени (§4.7)
};

struct UiConfig {
    bool liveMode = true;         // стартовать в live-режиме (§4.8, согласовано)
    int refreshHz = 4;            // частота перерисовки dashboard
    int eventsTail = 8;           // сколько последних событий в ленте
    bool showProgressBars = true;
    bool color = true;            // ANSI-цвета (откат в монохром вне TTY)
};

struct LoggingConfig {
    std::string level = "info";
    std::string file = "atm_sim.log";
};

// Полная конфигурация — всё вместе.
struct Config {
    AtmConfig atm;
    ClientsConfig clients;
    ServiceTimeConfig serviceTime;
    MaintenanceConfig maintenance;
    SimulationConfig simulation;
    UiConfig ui;
    LoggingConfig logging;
};

}  // namespace atmsim
