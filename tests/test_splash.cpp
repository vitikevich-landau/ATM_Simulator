// ============================================================================
//  test_splash.cpp — тесты экрана загрузки «самотест банкомата» (сцена v2).
//
//  Терминальный вывод (showSplash) в юнит-тестах не гоняем: вне TTY он
//  no-op по построению. Проверяем ЧИСТУЮ часть — сборку чек-листа из конфига:
//  именно там живут все подстановки реальных значений.
// ============================================================================
#include <string>

#include "atmsim/console/SplashScreen.hpp"
#include "simple_test.hpp"

using namespace atmsim;

namespace {

// Склейка чек-листа в один текст для поиска подстрок.
std::string flatten(const std::vector<SplashItem>& items) {
    std::string all;
    for (const SplashItem& it : items) all += it.name + " " + it.detail + "\n";
    return all;
}

}  // namespace

// Чек-лист отражает реальные значения конфига: касса с валютой, файл журнала,
// поток клиентов, ускорение времени, зерно ГСЧ.
TEST(splash_checklist_reflects_config) {
    Config cfg;
    cfg.atm.initialCash = 50'000'000;
    cfg.atm.currency = "EUR";
    cfg.logging.file = "atm_sim.log";
    cfg.clients.arrivalMode = ArrivalMode::Poisson;
    cfg.clients.arrivalRatePerMinute = 20.0;
    cfg.clients.count = 10;
    cfg.simulation.timeScale = 2.5;
    cfg.simulation.randomSeed = 4242;

    const std::vector<SplashItem> items = splashChecklist(cfg);
    CHECK(items.size() >= 5);
    for (const SplashItem& it : items) CHECK(!it.name.empty());

    const std::string all = flatten(items);
    CHECK(all.find("500 000.00 €") != std::string::npos);    // касса (EUR: символ + группировка разрядов)
    CHECK(all.find("atm_sim.log") != std::string::npos);     // журнал
    CHECK(all.find("poisson") != std::string::npos);         // режим прихода
    CHECK(all.find("20/мин") != std::string::npos);          // интенсивность
    CHECK(all.find("всего 10") != std::string::npos);        // клиентов за прогон
    CHECK(all.find("x2.5") != std::string::npos);            // time_scale
    CHECK(all.find("4242") != std::string::npos);            // зерно ГСЧ
}

// Режим batch и целые значения печатаются без хвостов («4», а не «4.0»).
TEST(splash_checklist_batch_and_compact_numbers) {
    Config cfg;
    cfg.clients.arrivalMode = ArrivalMode::Batch;
    cfg.clients.arrivalRatePerMinute = 4.0;
    cfg.simulation.timeScale = 1.0;

    const std::string all = flatten(splashChecklist(cfg));
    CHECK(all.find("batch") != std::string::npos);
    CHECK(all.find("4/мин") != std::string::npos);
    CHECK(all.find("x1\n") != std::string::npos || all.find("x1 ") != std::string::npos ||
          all.find("x1") != std::string::npos);
    CHECK(all.find("x1.0") == std::string::npos);
}
