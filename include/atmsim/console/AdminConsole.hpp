#pragma once
// ============================================================================
//  AdminConsole.hpp — интерактивная консоль администратора (M3, базовая).
//
//  Работает в ГЛАВНОМ потоке: читает строки из stdin, разбирает команду и
//  вызывает методы AtmEngine (снимки для отчётов, requestX для управления).
//  Пока это pull-модель «команда -> ответ» (§8). Живой дашборд (§4.8) — в M6.
// ============================================================================
#include "atmsim/config/Config.hpp"
#include "atmsim/engine/AtmEngine.hpp"

namespace atmsim {

class AdminConsole {
public:
    AdminConsole(AtmEngine& engine, const Config& cfg);

    // Главный цикл ввода команд. Возвращается по команде stop/exit/quit или EOF.
    void run();

private:
    void printHelp() const;
    void printStatus() const;
    void printQueue() const;

    AtmEngine& engine_;
    Config cfg_;
};

}  // namespace atmsim
