#include "atmsim/console/AdminConsole.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "atmsim/console/Signals.hpp"
#include "atmsim/console/Terminal.hpp"
#include "atmsim/core/Money.hpp"

namespace atmsim {
namespace {

// Форматирует момент времени журнала как HH:MM:SS (локальное время).
std::string formatTime(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    const std::tm* lt = std::localtime(&t);
    char buf[16];
    if (lt && std::strftime(buf, sizeof(buf), "%H:%M:%S", lt) > 0) return buf;
    return "??:??:??";
}

void printRecord(const OperationRecord& r) {
    std::cout << "  [" << formatTime(r.timestamp) << "] #" << r.clientId << ' ' << to_string(r.type);
    if (r.type != OperationType::CheckBalance) std::cout << ' ' << formatMoney(r.amount);
    if (r.success) std::cout << " — успех, баланс " << formatMoney(r.balanceAfter);
    else std::cout << " — отказ (" << r.errorMessage << ')';
    std::cout << '\n';
}

}  // namespace

AdminConsole::AdminConsole(AtmEngine& engine, const Config& cfg)
    : engine_(engine), cfg_(cfg) {}

// ---------------------------------------------------------------------------
//  Печать отчётов
// ---------------------------------------------------------------------------
void AdminConsole::printHelp() const {
    std::cout <<
        "Команды:\n"
        "  help                              — эта справка\n"
        "  status                            — общий статус банкомата\n"
        "  queue                             — список ожидающих в очереди\n"
        "  client <id>                       — отчёт по клиенту\n"
        "  balance <id>                      — баланс счёта клиента\n"
        "  operations [--client N] [--last N] [--type T]\n"
        "                                    — журнал операций (T: withdraw/deposit/check)\n"
        "  atm                               — состояние банкомата (касса, аптайм)\n"
        "  stats                             — статистика СМО\n"
        "  pause / resume                    — приостановить / возобновить обслуживание\n"
        "  maintenance start [сек] / stop    — техобслуживание\n"
        "  live / live off                   — включить / выключить живой дашборд\n"
        "  export <file>                     — выгрузить журнал операций в CSV\n"
        "  restart                           — новый прогон с нуля\n"
        "  stop                              — плавно остановить и выйти\n";
}

void AdminConsole::printStatus() const {
    const AtmSnapshot s = engine_.snapshot();
    std::cout << "=== Статус банкомата ===\n";
    std::cout << "Состояние:        " << to_string(s.state) << '\n';
    if (s.currentClientId) {
        std::cout << "Текущий клиент:   #" << *s.currentClientId
                  << " (" << to_string(*s.currentOperation) << ")\n";
    } else {
        std::cout << "Текущий клиент:   —\n";
    }
    std::cout << "В очереди:         " << s.queueLength
              << "  (макс. за прогон " << s.maxQueueLength << ")\n";
    std::cout << "Обслужено:         " << s.totalServed << '\n';
    std::cout << "Ушли (всего):      " << s.totalLeft << '\n';
    std::cout << "Касса:             " << formatMoney(s.cashboxBalance) << ' ' << cfg_.atm.currency
              << (s.lowCash ? "  [НИЗКАЯ КАССА]" : "") << '\n';
    if (s.state == AtmState::Maintenance) {
        std::cout << "ТО: ";
        if (s.maintenanceEtaSeconds < 0.0) std::cout << "до команды maintenance stop\n";
        else std::cout << "осталось ~" << static_cast<long>(s.maintenanceEtaSeconds) << " c\n";
    } else if (s.maintenancePending) {
        std::cout << "ТО: начнётся после завершения обслуживания текущего клиента\n";
    }
}

void AdminConsole::printQueue() const {
    const std::vector<ClientSnapshot> q = engine_.queueSnapshot();
    if (q.empty()) {
        std::cout << "Очередь пуста.\n";
        return;
    }
    std::cout << "В очереди " << q.size() << ":\n";
    for (std::size_t i = 0; i < q.size(); ++i) {
        const ClientSnapshot& c = q[i];
        std::cout << "  " << (i + 1) << ". #" << c.id << ' ' << to_string(c.requestedOperation);
        if (c.requestedOperation != OperationType::CheckBalance) std::cout << ' ' << formatMoney(c.amount);
        std::cout << "  ждёт " << static_cast<long>(c.waitedSeconds)
                  << " c, терпение осталось " << static_cast<long>(c.remainingPatience) << " c\n";
    }
}

void AdminConsole::printClient(ClientId id) const {
    const auto rep = engine_.clientReport(id);
    if (!rep) {
        std::cout << "Клиент #" << id << " не найден.\n";
        return;
    }
    std::cout << "=== Клиент #" << rep->id << " ===\n";
    std::cout << "Статус:      " << to_string(rep->state) << '\n';
    std::cout << "Операция:    " << to_string(rep->requestedOperation);
    if (rep->requestedOperation != OperationType::CheckBalance) std::cout << ' ' << formatMoney(rep->amount);
    std::cout << '\n';
    std::cout << "Терпение:    " << rep->patienceSeconds << " c\n";
    std::cout << "Баланс:      " << formatMoney(rep->accountBalance) << ' ' << cfg_.atm.currency << '\n';
    if (rep->history.empty()) {
        std::cout << "История операций: пусто\n";
    } else {
        std::cout << "История операций:\n";
        for (const auto& r : rep->history) printRecord(r);
    }
}

void AdminConsole::printBalance(ClientId id) const {
    const auto bal = engine_.balanceOf(id);
    if (!bal) {
        std::cout << "Клиент #" << id << " не найден.\n";
        return;
    }
    std::cout << "Баланс клиента #" << id << ": " << formatMoney(*bal) << ' ' << cfg_.atm.currency << '\n';
}

void AdminConsole::printOperations(const Command& c) const {
    OperationFilter f;
    f.client = c.clientId;
    f.type = c.opType;
    f.last = c.last;
    const std::vector<OperationRecord> ops = engine_.operations(f);
    if (ops.empty()) {
        std::cout << "Записей журнала не найдено.\n";
        return;
    }
    std::cout << "Операции (" << ops.size() << "):\n";
    for (const auto& r : ops) printRecord(r);
}

void AdminConsole::printAtm() const {
    const AtmSnapshot s = engine_.snapshot();
    std::cout << "=== Банкомат ===\n";
    std::cout << "Состояние:  " << to_string(s.state) << '\n';
    std::cout << "Касса:      " << formatMoney(s.cashboxBalance) << ' ' << cfg_.atm.currency
              << (s.lowCash ? "  [НИЗКАЯ КАССА]" : "") << '\n';
    std::cout << std::fixed << std::setprecision(0)
              << "Аптайм:     " << s.uptimeSeconds << " c модельного времени\n";
    std::cout.unsetf(std::ios::floatfield);
}

void AdminConsole::printStats() const {
    const StatsSnapshot s = engine_.statsSnapshot();
    std::cout << "=== Статистика СМО ===\n";
    std::cout << "Обслужено:            " << s.served << '\n';
    std::cout << "Ушли (всего):         " << s.left
              << "  (из них по ТО: " << s.renegedByMaintenance << ")\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Среднее ожидание:     " << s.avgWaitSeconds << " c\n";
    std::cout << "Среднее обслуживание: " << s.avgServiceSeconds << " c\n";
    std::cout << "Макс. длина очереди:  " << s.maxQueueLength << '\n';
    std::cout << std::setprecision(2) << "Загрузка ρ = λ/μ:     " << s.rhoTheoretical << '\n';
    std::cout << "Факт. загрузка:       " << s.utilization << '\n';
    std::cout << std::setprecision(0) << "Аптайм:               " << s.uptimeSeconds << " c\n";
    std::cout.unsetf(std::ios::floatfield);
}

void AdminConsole::doExport(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary);
    if (!out) {
        std::cout << "Не удалось открыть файл для записи: " << filename << '\n';
        return;
    }
    const std::vector<OperationRecord> ops = engine_.operations(OperationFilter{});
    out << "op_id,client_id,type,amount_minor,balance_after_minor,success,error\n";
    for (const auto& r : ops) {
        out << r.id << ',' << r.clientId << ',' << to_string(r.type) << ',' << r.amount << ','
            << r.balanceAfter << ',' << (r.success ? "1" : "0") << ',' << r.errorMessage << '\n';
    }
    std::cout << "Экспортировано записей: " << ops.size() << " -> " << filename << '\n';
}

bool AdminConsole::dispatchReport(const Command& c) const {
    switch (c.type) {
        case CommandType::Help:       printHelp(); return true;
        case CommandType::Status:     printStatus(); return true;
        case CommandType::Queue:      printQueue(); return true;
        case CommandType::Client:     printClient(*c.clientId); return true;
        case CommandType::Balance:    printBalance(*c.clientId); return true;
        case CommandType::Operations: printOperations(c); return true;
        case CommandType::Atm:        printAtm(); return true;
        case CommandType::Stats:      printStats(); return true;
        case CommandType::Export:     doExport(c.filename); return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
//  Главный цикл: переключение режимов
// ---------------------------------------------------------------------------
AdminConsole::RunOutcome AdminConsole::run() {
    ttyAnsi_ = Terminal::isStdoutTty();
    if (ttyAnsi_) Terminal::enableAnsi();
    if (cfg_.ui.liveMode && !ttyAnsi_) {
        std::cout << "(stdout не терминал — живой режим отключён, работаю в командном)\n";
    }

    Next next = (cfg_.ui.liveMode && ttyAnsi_) ? Next::Live : Next::Command;
    if (next == Next::Command) printHelp();

    while (next != Next::Quit && next != Next::Restart) {
        next = (next == Next::Live) ? runLiveSession() : runCommandLoop();
    }
    return (next == Next::Restart) ? RunOutcome::Restart : RunOutcome::Quit;
}

// ---------------------------------------------------------------------------
//  Командный режим (pull-REPL)
// ---------------------------------------------------------------------------
AdminConsole::Next AdminConsole::runCommandLoop() {
    std::cout << "\n[командный режим] help — команды, live — дашборд, stop — выход\n";
    std::string line;
    while (true) {
        // Плавная остановка по сигналу (§4.6): Ctrl+C взводит флаг и на Windows
        // разблокирует getline ниже (CancelSynchronousIo). Проверяем и здесь — на
        // случай, если сигнал пришёл вне блокирующего чтения.
        if (shutdownRequested()) return Next::Quit;
        if (engine_.allClientsProcessed()) {
            std::cout << "\n(все клиенты обслужены — restart для нового прогона, stop для выхода)\n";
        }
        std::cout << "\n> " << std::flush;
        if (!std::getline(std::cin, line)) return Next::Quit;
        if (shutdownRequested()) return Next::Quit;  // getline прерван сигналом

        const Command c = parseCommand(line);
        if (!c.error.empty()) {
            std::cout << "Ошибка: " << c.error << '\n';
            continue;
        }

        switch (c.type) {
            case CommandType::Empty: break;
            case CommandType::Pause:
                if (engine_.requestPause()) std::cout << "Обслуживание приостановлено.\n";
                else std::cout << "Пауза недоступна: идёт техобслуживание (или банкомат остановлен).\n";
                break;
            case CommandType::Resume: engine_.requestResume(); std::cout << "Обслуживание возобновлено.\n"; break;
            case CommandType::MaintenanceStart:
                // Ответ администратору — по фактическому результату: ТО могло
                // начаться сразу, а могло ждать конца текущего обслуживания (§4.5).
                switch (engine_.requestMaintenance(c.seconds)) {
                    case MaintenanceStart::Started:
                        std::cout << "Начато техобслуживание.\n"; break;
                    case MaintenanceStart::Deferred:
                        std::cout << "ТО начнётся после завершения обслуживания текущего клиента.\n"; break;
                    case MaintenanceStart::Ignored:
                        std::cout << "Банкомат остановлен — ТО не начато.\n"; break;
                }
                break;
            case CommandType::MaintenanceStop:  engine_.endMaintenance(); std::cout << "Техобслуживание завершено.\n"; break;
            case CommandType::Live:
                if (ttyAnsi_) return Next::Live;
                std::cout << "Живой режим недоступен: stdout не терминал.\n";
                break;
            case CommandType::LiveOff: break;  // уже в командном режиме
            case CommandType::Restart:
                std::cout << "Перезапуск прогона...\n";
                return Next::Restart;
            case CommandType::Stop:
                std::cout << "Останавливаюсь...\n";
                engine_.requestStop();
                return Next::Quit;
            case CommandType::Unknown:
                std::cout << "Неизвестная команда: '" << c.word << "'. Наберите help.\n";
                break;
            default:
                dispatchReport(c);  // отчёты и справка
                break;
        }
        if (engine_.isStopped()) return Next::Quit;
    }
}

// ---------------------------------------------------------------------------
//  Живой режим (дашборд + ввод внизу)
// ---------------------------------------------------------------------------
void AdminConsole::showOverlay(LiveRenderer& renderer, const std::function<void()>& printFn) {
    // Приостанавливаем перерисовку, показываем ответ на весь экран, ждём Enter.
    renderer.pause();
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::clearScreen() << ansi::home() << std::flush;
    }
    {
        // Печать отчёта — ПОД тем же outputMutex_, что и кадры рендерера. Иначе
        // «долетевший» кадр (см. повторную проверку paused_ в paintFrame) писал бы
        // в std::cout параллельно с printFn и символьно перемешивался с отчётом.
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        printFn();
    }
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << "\n-- нажмите Enter, чтобы вернуться к дашборду --" << std::flush;
    }
    std::string dummy;
    std::getline(std::cin, dummy);
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::clearScreen() << std::flush;
    }
    renderer.resume();
}

void AdminConsole::showQueueInteractive(LiveRenderer& renderer) {
    RawInputMode raw;  // stdin в raw-режим на время просмотра (восстановится в дтор)
    // Если raw-режим недоступен (stdout — терминал, а stdin — нет и т.п.) — не
    // ломаемся, а деградируем до обычного статического ответа с выходом по Enter.
    if (!raw.active()) {
        showOverlay(renderer, [&] { printQueue(); });
        return;
    }

    renderer.pause();  // рендер дашборда молчит — экраном владеет просмотрщик
    // Один раз гасим экран (на нём остался последний кадр дашборда). Дальше
    // перерисовываем БЕЗ clearScreen — через home()+clearToLineEnd на каждой
    // строке, — иначе живая перерисовка (см. readKeyTimeout ниже) мигала бы.
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::hideCursor() << ansi::clearScreen() << std::flush;
    }
    // Период перерисовки — как у дашборда (refresh_hz). По этому таймауту читаем
    // клавишу: нажали — реагируем; не нажали — просто перечитываем очередь и
    // перерисовываем. Именно это делает список ЖИВЫМ, а не застывшим до первого
    // нажатия (раньше readKey блокировал цикл, и очередь «замерзала» — особенно
    // сразу после restart, когда свежий движок ещё не набрал очередь).
    const int refreshMs = 1000 / std::max(1, cfg_.ui.refreshHz);
    int offset = 0;    // индекс первого видимого клиента очереди
    bool leave = false;
    while (!leave) {
        // Живые данные: очередь перечитывается на каждой перерисовке, а
        // перерисовка идёт по таймауту readKeyTimeout, а не только по нажатию.
        const std::vector<ClientSnapshot> q = engine_.queueSnapshot();
        const int total = static_cast<int>(q.size());
        const int viewRows = std::max(5, Terminal::height() - 4);  // строк под список
        offset = clampScrollOffset(offset, total, viewRows);

        {
            std::lock_guard<std::mutex> lk(renderer.outputMutex());
            std::ostringstream os;
            // home() вместо clearScreen(): каждую строку затираем до конца
            // (clearToLineEnd) — экран не мигает при перерисовке ~refresh_hz раз/с.
            // Кадр (шапка + viewRows строк + подвал) заполняет высоту терминала
            // целиком, поэтому «хвостов» от прошлого кадра не остаётся; при
            // ресайзе viewRows пересчитается на следующем же кадре.
            os << ansi::hideCursor() << ansi::home();  // курсор не мельтешит
            os << ansi::bold() << "Очередь: " << total << " клиент(ов)" << ansi::reset();
            if (total > viewRows) {
                os << ansi::grey() << "   показаны " << (offset + 1) << "–"
                   << std::min(offset + viewRows, total) << " из " << total << ansi::reset();
            }
            os << ansi::clearToLineEnd() << '\n' << ansi::clearToLineEnd() << '\n';
            for (int i = 0; i < viewRows; ++i) {
                const int idx = offset + i;
                if (idx >= total) { os << ansi::clearToLineEnd() << '\n'; continue; }  // пустой слот
                const ClientSnapshot& c = q[static_cast<std::size_t>(idx)];
                os << "  " << (idx + 1) << ". #" << c.id << ' ' << to_string(c.requestedOperation);
                if (c.requestedOperation != OperationType::CheckBalance) {
                    os << ' ' << formatMoney(c.amount);
                }
                os << "  ждёт " << static_cast<long>(c.waitedSeconds)
                   << " c, терпение " << static_cast<long>(c.remainingPatience) << " c"
                   << ansi::clearToLineEnd() << '\n';
            }
            os << ansi::clearToLineEnd() << '\n' << ansi::grey()
               << "↑/↓ прокрутка · PgUp/PgDn страница · Home/End · Esc/q выход" << ansi::reset()
               << ansi::clearToLineEnd();
            std::cout << os.str() << std::flush;
        }

        char ch = 0;
        switch (readKeyTimeout(ch, refreshMs)) {
            case Key::Up:       offset -= 1; break;
            case Key::Down:     offset += 1; break;
            case Key::PageUp:   offset -= viewRows; break;
            case Key::PageDown: offset += viewRows; break;
            case Key::Home:     offset = 0; break;
            case Key::End:      offset = total; break;  // клампнётся к максимуму ниже
            case Key::Enter:
            case Key::Escape:
            case Key::Eof:      leave = true; break;
            case Key::Char:     if (ch == 'q' || ch == 'Q') leave = true; break;
            default: break;  // None (таймаут — просто перерисуем на след. круге) и прочее
        }
        // Плавная остановка (§4.6): Ctrl+C взводит флаг в обработчике сигнала.
        // Раньше цикл висел в блокирующем readKey и проверить флаг не мог (на
        // POSIX выходил лишь случайно: EINTR превращался в Eof, что select теперь
        // съедает как таймаут). Тикаем каждые refreshMs — проверяем явно, выходим
        // не позже чем через один период перерисовки на обеих платформах.
        if (shutdownRequested()) leave = true;
    }

    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::clearScreen() << ansi::showCursor() << std::flush;  // курсор обратно
    }
    renderer.resume();
}

bool AdminConsole::readCommandLineRaw(LiveRenderer& renderer, int inputRow, std::string& out) {
    // Сигнал мог прийти, пока мы были НЕ в чтении (например, в просмотре очереди,
    // который выходит по флагу). Без этой проверки мы бы снова заблокировались в
    // readKey/getline и потребовали от пользователя ещё одно нажатие для выхода.
    if (shutdownRequested()) { out.clear(); return false; }
    RawInputMode raw;
    if (!raw.active()) {
        // stdin не терминал — без raw-редактора, обычный построчный ввод.
        {
            std::lock_guard<std::mutex> lk(renderer.outputMutex());
            std::cout << ansi::moveTo(inputRow, 1) << ansi::clearToLineEnd() << "cmd> " << std::flush;
        }
        return static_cast<bool>(std::getline(std::cin, out));
    }

    const std::string prompt = "cmd> ";
    std::string buf;
    std::size_t cur = 0;
    for (;;) {
        // Рисуем строку ввода САМИ (в raw-режиме терминал не эхоит) и ставим курсор
        // в позицию редактирования. Всё под outputMutex_ — синхронно с кадрами.
        // Ту же позицию сообщаем рендереру, чтобы его кадры возвращали курсор сюда
        // же, а не в таблицу. Колонку курсора считаем по ОТОБРАЖАЕМЫМ символам
        // (displayColumns), а не по байтам — иначе кириллица (2 байта/символ)
        // уводила бы курсор вправо. prompt — ASCII, его длина = число колонок.
        const int cursorCol =
            static_cast<int>(prompt.size() + displayColumns(buf, cur)) + 1;
        renderer.setCursorTarget(inputRow, cursorCol);
        {
            std::lock_guard<std::mutex> lk(renderer.outputMutex());
            std::ostringstream os;
            os << ansi::moveTo(inputRow, 1) << ansi::clearToLineEnd() << prompt << buf
               << ansi::moveTo(inputRow, cursorCol);
            std::cout << os.str() << std::flush;
        }
        char ch = 0;
        const Key k = readKey(ch);
        // Плавный выход по Ctrl+C (§4.6). В live-режиме ключи читает _getch, его
        // сигнал не прерывает, поэтому реагируем при следующем нажатии клавиши.
        if (shutdownRequested()) { out.clear(); return false; }
        const LineEdit r = editLine(buf, cur, k, ch);
        if (r == LineEdit::Submit) { out = buf; return true; }
        if (r == LineEdit::Cancel) { out.clear(); return false; }
    }
}

AdminConsole::Next AdminConsole::runLiveSession() {
    LiveRenderer renderer(engine_, cfg_);
    const int inputRow = renderer.height() + 2;  // строка ввода — НИЖЕ дашборда
    // Куда рендер-поток будет возвращать курсор: строка ввода, сразу за «cmd> ».
    // Задаём ДО start(), чтобы даже первый кадр не поставил курсор в таблицу.
    renderer.setCursorTarget(inputRow, 6);

    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::altScreenOn() << ansi::clearScreen() << std::flush;
    }
    renderer.start();

    Next result = Next::Command;
    std::string line;
    bool exitLoop = false;
    while (!exitLoop) {
        // Ввод команды — в raw-режиме с собственным эхом (readCommandLineRaw рисует
        // строку ввода сам, под outputMutex_). Так набор не конфликтует с кадрами
        // рендерера: символы не пропадают, приглашение не портится, работает
        // редактирование ←/→/Backspace (§4.8: раньше cooked-эхо давало артефакты).
        if (!readCommandLineRaw(renderer, inputRow, line)) { result = Next::Quit; break; }

        const Command c = parseCommand(line);
        if (!c.error.empty()) {
            showOverlay(renderer, [&] { std::cout << "Ошибка: " << c.error << '\n'; });
            continue;
        }

        switch (c.type) {
            case CommandType::Empty: break;
            case CommandType::Pause:  engine_.requestPause();  break;  // видно на дашборде
            case CommandType::Resume: engine_.requestResume(); break;
            case CommandType::MaintenanceStart: engine_.requestMaintenance(c.seconds); break;
            case CommandType::MaintenanceStop:  engine_.endMaintenance(); break;
            case CommandType::Live: break;  // уже в живом режиме
            case CommandType::LiveOff: result = Next::Command; exitLoop = true; break;
            case CommandType::Queue:
                // В живом режиме очередь листаем интерактивно (стрелки/PgUp/PgDn),
                // а не показываем статичным полноэкранным списком.
                showQueueInteractive(renderer);
                break;
            case CommandType::Restart:
                // Выходим с исходом Restart; текущий движок остановит и пересоздаст
                // main (мы лишь сигналим о намерении).
                result = Next::Restart;
                exitLoop = true;
                break;
            case CommandType::Stop:
                engine_.requestStop();
                result = Next::Quit;
                exitLoop = true;
                break;
            case CommandType::Unknown:
                showOverlay(renderer, [&] { std::cout << "Неизвестная команда: '" << c.word << "'.\n"; });
                break;
            default:
                // Отчёт/справка — полноэкранным ответом поверх дашборда.
                showOverlay(renderer, [&] { dispatchReport(c); });
                break;
        }
        if (!exitLoop && engine_.isStopped()) { result = Next::Quit; break; }
    }

    renderer.stop();
    {
        std::lock_guard<std::mutex> lk(renderer.outputMutex());
        std::cout << ansi::altScreenOff() << ansi::showCursor() << std::flush;
    }
    return result;
}

}  // namespace atmsim
