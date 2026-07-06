#pragma once
// ============================================================================
//  AdminConsole.hpp — интерактивная консоль администратора (M4).
//
//  Работает в ГЛАВНОМ потоке: читает строки из stdin, разбирает их через
//  CommandParser и вызывает методы AtmEngine (снимки/отчёты для чтения,
//  requestX для управления). Пока это pull-модель «команда -> ответ» (§8);
//  живой дашборд (§4.8) — в M6.
// ============================================================================
#include <string>

#include "atmsim/config/Config.hpp"
#include "atmsim/console/CommandParser.hpp"
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
    void printClient(ClientId id) const;
    void printBalance(ClientId id) const;
    void printOperations(const Command& c) const;
    void printAtm() const;
    void printStats() const;
    void doExport(const std::string& filename) const;

    AtmEngine& engine_;
    Config cfg_;
};

}  // namespace atmsim
