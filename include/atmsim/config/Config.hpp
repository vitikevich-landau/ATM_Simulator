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
    std::string currency = "EUR";        // код валюты (EUR/USD/RUB/…)
    // Переопределение формата валюты из конфига (atm.currency_format): символ и
    // его позиция, разделитель разрядов, десятичный знак, число знаков. Пусто ->
    // формат целиком из встроенной таблицы по коду currency (resolveCurrencyFormat).
    CurrencyOverride currencyOverride;
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

// Время ПОДХОДА клиента к банкомату (МОДЕЛЬНЫЕ секунды): взятый из очереди
// клиент сначала идёт к терминалу и только потом начинается обслуживание
// (снимок в это время отдаёт approaching/approachProgress, а не этап).
// Разыгрывается равномерно из [min, max]; min == max — фиксированное время.
// По умолчанию 0/0 — подход мгновенный (поведение до этой фичи): так
// программные конфиги и тесты не меняют семантику незаметно для себя;
// «живое» значение задаёт config/default_config.json.
struct WalkSecondsRange {
    double min = 0.0;
    double max = 0.0;
};

struct ClientsConfig {
    ArrivalMode arrivalMode = ArrivalMode::Poisson;
    double arrivalRatePerMinute = 4.0;   // для poisson: среднее число в минуту
    OperationWeights weights;
    AmountRange amountRange;
    SecondsRange patienceSeconds;
    WalkSecondsRange walkSeconds;

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
    // --- Анимированная сцена (feature/scene) ---------------------------------
    // Полоса с банкоматом и человечками НАД таблицей дашборда. Требует
    // терминал от ~84x30; при меньшем окне рендерер молча остаётся в
    // табличном режиме (см. LiveRenderer::sceneActive_).
    bool scene = false;
    int sceneFps = 15;            // частота кадров сцены, 5..30 (задействуется этапом 5)
    int sceneRows = 10;           // высота сценической полосы, 10..14 строк
    bool sceneEffects = true;     // спецэффекты: спиннер связи, купюры, «пар» злости
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
