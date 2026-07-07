#pragma once
// ============================================================================
//  AdminConsole.hpp — консоль администратора (M6): командный режим + живой дашборд.
//
//  Два режима (§4.8.1):
//    * command — классический REPL «команда -> ответ» (§8);
//    * live    — полноэкранный дашборд, автообновление (§4.8), ввод команд внизу.
//  Стартовый режим — из конфига (ui.live_mode), но live доступен ТОЛЬКО если
//  stdout — интерактивный терминал; иначе (пайп/файл) — командный режим (§4.8.7).
// ============================================================================
#include <functional>
#include <string>

#include "atmsim/config/Config.hpp"
#include "atmsim/console/CommandParser.hpp"
#include "atmsim/console/LiveRenderer.hpp"
#include "atmsim/engine/AtmEngine.hpp"

namespace atmsim {

class AdminConsole {
public:
    AdminConsole(AtmEngine& engine, const Config& cfg);

    // Главный цикл: переключается между командным и живым режимами, пока не stop/EOF.
    void run();

private:
    // Что делать после выхода из одного из режимов.
    enum class Next { Command, Live, Quit };

    Next runCommandLoop();   // pull-REPL; -> Live (по live) или Quit
    Next runLiveSession();   // живой дашборд; -> Command (по live off) или Quit

    // Печать отчётов (пишут в std::cout). Разделены, чтобы переиспользоваться и в
    // командном режиме, и как «полноэкранный ответ» (overlay) в живом режиме.
    void printHelp() const;
    void printStatus() const;
    void printQueue() const;
    void printClient(ClientId id) const;
    void printBalance(ClientId id) const;
    void printOperations(const Command& c) const;
    void printAtm() const;
    void printStats() const;
    void doExport(const std::string& filename) const;

    // Выполняет команду-отчёт/справку (печать в cout). true — если это была
    // именно отчётная команда (иначе — управляющая, обрабатывается отдельно).
    bool dispatchReport(const Command& c) const;

    // В живом режиме показывает результат разовой команды поверх дашборда:
    // приостанавливает рендер, печатает, ждёт Enter, восстанавливает дашборд.
    void showOverlay(LiveRenderer& renderer, const std::function<void()>& printFn);

    // Интерактивный просмотр очереди в живом режиме: raw-режим ввода, прокрутка
    // стрелками (↑/↓, PgUp/PgDn, Home/End), выход по Esc/q/Enter. Очередь
    // перечитывается на каждую перерисовку (живые данные). Если raw-режим
    // недоступен (не терминал) — деградирует до статического списка.
    void showQueueInteractive(LiveRenderer& renderer);

    AtmEngine& engine_;
    Config cfg_;
    bool ttyAnsi_{false};  // stdout — интерактивный терминал (можно live-режим)
};

}  // namespace atmsim
