#include "atmsim/console/SplashScreen.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <thread>

#include "atmsim/Version.hpp"
#include "atmsim/console/Signals.hpp"
#include "atmsim/console/Terminal.hpp"
#include "atmsim/core/Money.hpp"

namespace atmsim {
namespace {

// Ритм заставки: пункт «проверяется» kRevealMsPerItem, спиннер перерисовывается
// каждый кадр, финальное «ГОТОВ К РАБОТЕ» висит kFinalHoldMs. Итого при семи
// пунктах — примерно 2.4 секунды; любая клавиша обрывает мгновенно.
constexpr int kRevealMsPerItem = 260;
constexpr int kFrameMs = 40;  // ~25 кадров/с
constexpr int kFinalHoldMs = 600;
// Имя пункта добивается точками до этой колонки (в кодовых точках).
constexpr int kNameFieldCols = 26;
// Ширина внутренней части рамки-шапки, колонок.
constexpr int kBoxInnerCols = 44;

// Спиннер — только ASCII: раскладка глифов в legacy-шрифтах непредсказуема
// (см. историю со спиннером сцены на MSVC).
constexpr char kSpinner[] = {'|', '/', '-', '\\'};

// Число без хвостовых нулей («20», «2.5») — интенсивность, time_scale.
std::string compactNumber(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

// Ширина строки в колонках терминала = кодовых точках UTF-8 (кириллица в
// байтах вдвое длиннее — size() дал бы кривое выравнивание).
std::size_t columnsOf(const std::string& s) {
    return displayColumns(s, s.size());
}

// Строка, центрированная в поле width колонок.
std::string centered(const std::string& s, int width) {
    const int pad = (width - static_cast<int>(columnsOf(s))) / 2;
    const int left = pad > 0 ? pad : 0;
    const int right = width - static_cast<int>(columnsOf(s)) - left;
    return std::string(static_cast<std::size_t>(left), ' ') + s +
           std::string(static_cast<std::size_t>(right > 0 ? right : 0), ' ');
}

}  // namespace

std::vector<SplashItem> splashChecklist(const Config& cfg) {
    std::vector<SplashItem> items;
    items.push_back({"инициализация терминала", ""});
    items.push_back(
        {"загрузка кассет",
         formatMoney(cfg.atm.initialCash,
                     resolveCurrencyFormat(cfg.atm.currency, cfg.atm.currencyOverride))});
    items.push_back({"связь с процессингом", ""});
    items.push_back({"журнал операций", cfg.logging.file});
    {
        std::ostringstream os;
        os << (cfg.clients.arrivalMode == ArrivalMode::Poisson ? "poisson" : "batch") << ", "
           << compactNumber(cfg.clients.arrivalRatePerMinute) << "/мин, всего "
           << cfg.clients.count;
        items.push_back({"генератор клиентов", os.str()});
    }
    items.push_back({"модельное время", "x" + compactNumber(cfg.simulation.timeScale)});
    items.push_back({"зерно ГСЧ", std::to_string(cfg.simulation.randomSeed)});
    return items;
}

void showSplash(const Config& cfg) {
    // Вне интерактивного терминала (пайп, CI, перенаправление) заставка не
    // имеет смысла и только сорила бы ANSI-кодами. Сигнал остановки, пришедший
    // до старта, тоже отменяет показ.
    if (!Terminal::isStdoutTty() || shutdownRequested()) return;
    // Администратор уже печатает следующую команду (type-ahead после restart)?
    // Не показываем заставку вовсе: её «пропуск по любой клавише» съел бы
    // первый символ набранной команды, и getline получил бы огрызок.
    if (Terminal::inputPending()) return;
    Terminal::enableAnsi();
    const bool color = cfg.ui.color;
    const auto C = [color](const char* code) { return color ? code : ""; };

    const std::vector<SplashItem> items = splashChecklist(cfg);
    // Кадр заставки занимает шапку (6 строк), пункты и подвал (4 строки);
    // на терминале ниже — молча пропускаем (тот же принцип «не влезает —
    // остаёмся без украшений», что у сцены дашборда).
    if (Terminal::height() < static_cast<int>(items.size()) + 10) return;
    RawInputMode raw;  // пропуск по любой клавише (если stdin интерактивен)

    // Заставка живёт в альтернативном буфере, как и live-режим: после неё
    // основной буфер остаётся нетронутым (баннер запуска не затирается).
    std::cout << ansi::altScreenOn() << ansi::hideCursor() << ansi::clearScreen();

    // Кадр: пункты 0..done-1 — OK, пункт done «проверяется» (спиннер),
    // остальные строки пустые (высота кадра постоянна — ничего не ёрзает).
    // done == items.size() — финальный кадр с «ГОТОВ К РАБОТЕ».
    const auto drawFrame = [&](std::size_t done, int spinnerPhase) {
        std::ostringstream f;
        f << ansi::syncBegin() << ansi::home() << '\n';
        const std::string pad = "   ";
        const std::string bar(static_cast<std::size_t>(kBoxInnerCols), ' ');

        f << pad << "┌";
        for (int i = 0; i < kBoxInnerCols; ++i) f << "─";
        f << "┐" << ansi::clearToLineEnd() << '\n';
        f << pad << "│" << C(ansi::bold())
          << centered(std::string(kProjectName) + " v" + kVersion, kBoxInnerCols)
          << C(ansi::reset()) << "│" << ansi::clearToLineEnd() << '\n';
        f << pad << "│" << C(ansi::grey()) << centered("самотестирование банкомата", kBoxInnerCols)
          << C(ansi::reset()) << "│" << ansi::clearToLineEnd() << '\n';
        f << pad << "└";
        for (int i = 0; i < kBoxInnerCols; ++i) f << "─";
        f << "┘" << ansi::clearToLineEnd() << '\n';
        f << ansi::clearToLineEnd() << '\n';

        for (std::size_t i = 0; i < items.size(); ++i) {
            f << pad << ' ';
            if (i > done) {
                // Ещё не дошли: пустая строка держит высоту кадра.
                f << ansi::clearToLineEnd() << '\n';
                continue;
            }
            const SplashItem& it = items[i];
            f << it.name << ' ' << C(ansi::grey());
            for (int d = static_cast<int>(columnsOf(it.name)); d < kNameFieldCols; ++d) f << '.';
            f << C(ansi::reset()) << ' ';
            if (i == done) {
                f << C(ansi::cyan()) << kSpinner[spinnerPhase % 4] << C(ansi::reset());
            } else {
                f << C(ansi::green()) << "OK" << C(ansi::reset());
                if (!it.detail.empty()) {
                    f << "  " << C(ansi::grey()) << it.detail << C(ansi::reset());
                }
            }
            f << ansi::clearToLineEnd() << '\n';
        }

        f << ansi::clearToLineEnd() << '\n';
        f << pad << ' ';
        if (done >= items.size()) {
            f << C(ansi::green()) << C(ansi::bold()) << "ГОТОВ К РАБОТЕ" << C(ansi::reset());
        }
        f << ansi::clearToLineEnd() << '\n';
        f << ansi::clearToLineEnd() << '\n';
        // Последняя строка БЕЗ '\n': на терминале ровно в высоту кадра перевод
        // строки с нижнего ряда прокручивал бы альтернативный буфер на каждом
        // кадре — заставка «тряслась» бы все ~2.4 секунды.
        f << pad << ' ' << C(ansi::grey()) << "любая клавиша — пропустить" << C(ansi::reset())
          << ansi::clearToLineEnd();
        f << ansi::syncEnd();
        std::cout << f.str() << std::flush;
    };

    // Один шаг ожидания: клавиша (включая EOF закрытого stdin) или сигнал
    // остановки обрывают заставку; без интерактивного stdin — просто спим.
    const auto interrupted = [&](int ms) {
        if (shutdownRequested()) return true;
        if (raw.active()) {
            char ch = 0;
            return readKeyTimeout(ch, ms) != Key::None;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return false;
    };

    bool skipped = false;
    for (std::size_t done = 0; done < items.size() && !skipped; ++done) {
        for (int t = 0; t < kRevealMsPerItem && !skipped; t += kFrameMs) {
            drawFrame(done, t / kFrameMs);
            skipped = interrupted(kFrameMs);
        }
    }
    if (!skipped) {
        drawFrame(items.size(), 0);
        for (int t = 0; t < kFinalHoldMs && !skipped; t += kFrameMs) {
            skipped = interrupted(kFrameMs);
        }
    }

    // Дожимаем хвост ввода (автоповтор зажатой клавиши, лишние нажатия):
    // иначе он просочился бы в строку команд следующего экрана. Eof тоже
    // выходит из цикла — закрытый stdin возвращал бы его бесконечно.
    if (raw.active()) {
        for (;;) {
            char ch = 0;
            const Key k = readKeyTimeout(ch, 0);
            if (k == Key::None || k == Key::Eof) break;
        }
    }

    std::cout << ansi::showCursor() << ansi::altScreenOff();
    std::cout.flush();
}

}  // namespace atmsim
