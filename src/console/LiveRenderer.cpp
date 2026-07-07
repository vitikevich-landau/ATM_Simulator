#include "atmsim/console/LiveRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "atmsim/console/Terminal.hpp"
#include "atmsim/core/Money.hpp"

namespace atmsim {
namespace {

// Форматирует секунды как HH:MM:SS.
std::string hms(double seconds) {
    // long long (>= 64 бит везде), а не long: на Windows (LLP64) long — 32 бита,
    // и модельный аптайм (реальные секунды * time_scale) при заметном ускорении
    // времени переполнил бы его за часы прогона, дав отрицательное HH:MM:SS.
    long long total = static_cast<long long>(seconds < 0 ? 0 : seconds);
    std::ostringstream os;
    os << std::setfill('0') << std::setw(2) << (total / 3600) << ':'
       << std::setw(2) << ((total % 3600) / 60) << ':' << std::setw(2) << (total % 60);
    return os.str();
}

// Полоса-индикатор шириной width по доле frac (0..1): залито '█', пусто '░'.
std::string bar(double frac, int width) {
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    const int filled = static_cast<int>(std::lround(frac * width));
    std::string s;
    for (int i = 0; i < width; ++i) s += (i < filled) ? "█" : "░";
    return s;
}

// Повторяет UTF-8 символ n раз (для разделителей из '─').
std::string repeatUtf8(const char* glyph, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += glyph;
    return s;
}

// Подгоняет строку под ширину width: короткую дополняет пробелами, длинную
// усекает (ANSI-последовательности не режет, в конце усечения закрывает цвет).
std::string fit(const std::string& s, int width) {
    std::string out;
    int w = 0;
    bool truncated = false;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1B) {  // ANSI — копируем целиком, ширина 0
            std::size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                ++j;
                while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
                if (j < s.size()) ++j;
            }
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (w >= width) { truncated = true; break; }
        int bytes = 1;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        out.append(s, i, static_cast<std::size_t>(bytes));
        ++w;
        i += static_cast<std::size_t>(bytes);
    }
    // Если усекли окрашенную строку — закрываем цвет, чтобы он не «потёк» дальше.
    if (truncated && out.find('\033') != std::string::npos) out += "\033[0m";
    else for (; w < width; ++w) out += ' ';
    return out;
}

}  // namespace

LiveRenderer::LiveRenderer(AtmEngine& engine, const Config& cfg)
    : engine_(engine), cfg_(cfg) {
    // Размеры терминала — один раз при создании (ресайз на лету — v2).
    width_ = std::clamp(Terminal::width(), 60, 200);
    termHeight_ = std::clamp(Terminal::height(), 16, 60);
    height_ = static_cast<int>(composeLines().size());
}

std::vector<std::string> LiveRenderer::composeLines() const {
    const bool color = cfg_.ui.color;
    auto C = [color](const char* code) { return color ? std::string(code) : std::string(); };
    auto R = [color] { return color ? std::string(ansi::reset()) : std::string(); };

    // Сумма со знаком и цветом по направлению: внесение — зелёным «+», снятие —
    // красным «−»; для проверки баланса суммы нет.
    auto signedAmount = [&](OperationType op, Money amount) -> std::string {
        if (op == OperationType::Deposit)  return C(ansi::green()) + "+" + formatMoney(amount) + R();
        if (op == OperationType::Withdraw) return C(ansi::red())   + "-" + formatMoney(amount) + R();
        return std::string{};
    };

    const AtmSnapshot s = engine_.snapshot();
    const StatsSnapshot st = engine_.statsSnapshot();
    const std::vector<ClientSnapshot> q = engine_.queueSnapshot();
    // Длина ленты последних операций — из конфига (ui.events_tail), с разумным
    // потолком, чтобы битый конфиг не растянул кадр до абсурда. Значение постоянно
    // в пределах запуска, поэтому высота кадра остаётся стабильной (§4.8.5).
    const int feedRows = std::clamp(cfg_.ui.eventsTail, 0, 50);
    OperationFilter feedFilter;
    feedFilter.last = static_cast<std::size_t>(feedRows);
    const std::vector<OperationRecord> ops = engine_.operations(feedFilter);

    const std::string cur = cfg_.atm.currency;

    // Геометрия: левая колонка (очередь) шире, правая — статистика/лента.
    const int leftW = std::clamp(width_ - 42, 40, 54);
    const int rightW = width_ - leftW - 3;              // 3 = " │ "
    const int queueVisible = std::max(4, termHeight_ - 13);

    // Цвет маркера состояния.
    const char* sc = ansi::grey();
    switch (s.state) {
        case AtmState::Serving:     sc = ansi::green();  break;
        case AtmState::Paused:      sc = ansi::yellow(); break;
        case AtmState::Maintenance: sc = ansi::blue();   break;
        case AtmState::Stopped:     sc = ansi::grey();   break;
        case AtmState::Idle:        sc = ansi::grey();   break;
    }

    std::vector<std::string> L;

    // === Полноширинная шапка ===
    {
        std::ostringstream os;
        os << C(ansi::bold()) << "ATM Simulator" << R()
           << "   uptime " << hms(s.uptimeSeconds) << "   [LIVE " << cfg_.ui.refreshHz << " fps]";
        L.push_back(fit(os.str(), width_));
    }
    {
        std::ostringstream os;
        os << "Состояние: " << C(sc) << to_string(s.state) << R();
        if (s.state == AtmState::Maintenance) {
            os << " (";
            if (s.maintenanceEtaSeconds < 0.0) os << "до stop";
            else os << "~" << static_cast<long>(s.maintenanceEtaSeconds) << "c";
            os << ")";
        }
        const double cashFrac = (cfg_.atm.initialCash > 0)
            ? static_cast<double>(s.cashboxBalance) / static_cast<double>(cfg_.atm.initialCash) : 0.0;
        // Цвет ЧИСЛА кассы — по направлению последней операции: внесли -> зелёным,
        // сняли -> красным (держится, пока направление не сменится). Полоса же
        // показывает уровень заполнения (красная при низкой кассе).
        const char* cashColor = (s.lastCashMove == OperationType::Withdraw) ? ansi::red() : ansi::green();
        os << "   Касса ";
        // Полоса заполнения кассы — только если ui.show_progress_bars включён;
        // иначе оставляем само число (амаунт и так показан ниже).
        if (cfg_.ui.showProgressBars) {
            os << "[" << (s.lowCash ? C(ansi::red()) : C(ansi::green())) << bar(cashFrac, 16) << R()
               << "] ";
        }
        os << C(cashColor) << formatMoney(s.cashboxBalance) << R() << ' ' << cur
           << (s.lowCash ? std::string(" ") + C(ansi::red()) + "НИЗКАЯ" + R() : std::string{});
        L.push_back(fit(os.str(), width_));
    }

    // === Разделитель с «шапкой» колонок (┬) ===
    L.push_back(C(ansi::grey()) + repeatUtf8("─", leftW + 1) + "┬" +
                repeatUtf8("─", width_ - leftW - 2) + R());

    // === Левая колонка: кто обслуживается + очередь ===
    std::vector<std::string> left;
    {
        std::ostringstream os;
        os << "Обслуживается: ";
        if (s.currentClientId) {
            os << C(ansi::cyan()) << '#' << *s.currentClientId << R() << ' '
               << to_string(*s.currentOperation);
        } else {
            os << C(ansi::grey()) << "— (простой)" << R();
        }
        left.push_back(os.str());
    }
    left.push_back("");
    {
        std::ostringstream os;
        os << "Очередь (" << s.queueLength << ")   макс. " << s.maxQueueLength;
        left.push_back(os.str());
    }
    // ВАЖНО: всегда выводим РОВНО queueVisible строк очереди (пустые слоты —
    // пустой строкой) и ВСЕГДА строку-хвост переполнения (пустую, если
    // переполнения нет). Так высота дашборда ПОСТОЯННА и не зависит от длины
    // очереди. Иначе плавающая высота ломала раскладку: height()/строку ввода
    // (inputRow = height()+2) считаем один раз, и при росте/сжатии кадра нижний
    // «хвост» (строка команд) налезал на ввод и дублировался (§4.8.5).
    for (int i = 0; i < queueVisible; ++i) {
        if (i >= static_cast<int>(q.size())) { left.push_back(std::string{}); continue; }
        const ClientSnapshot& c = q[static_cast<std::size_t>(i)];
        const double total = c.waitedSeconds + c.remainingPatience;
        const double frac = (total > 0.0) ? c.remainingPatience / total : 0.0;
        const char* pc = (frac > 0.4) ? ansi::green() : (frac > 0.15) ? ansi::yellow() : ansi::red();
        std::ostringstream os;
        os << " #" << c.id << ' ' << to_string(c.requestedOperation);
        const std::string amt = signedAmount(c.requestedOperation, c.amount);
        if (!amt.empty()) os << ' ' << amt;
        os << "  " << static_cast<long>(c.waitedSeconds) << "c ";
        // Полоса остатка терпения — только при ui.show_progress_bars; иначе просто
        // «ждал Nc / осталось Mc» без визуальной шкалы.
        if (cfg_.ui.showProgressBars) {
            os << "[" << C(pc) << bar(frac, 4) << R() << "] ";
        }
        os << static_cast<long>(c.remainingPatience) << 'c';
        left.push_back(os.str());
    }
    left.push_back(q.size() > static_cast<std::size_t>(queueVisible)
        ? C(ansi::grey()) + " … ещё " + std::to_string(q.size() - queueVisible) + " (команда queue)" + R()
        : std::string{});

    // === Правая колонка: статистика + лента ===
    std::vector<std::string> right;
    right.push_back(C(ansi::bold()) + "СТАТИСТИКА" + R());
    {
        std::ostringstream os; os << " Обслужено:   " << st.served; right.push_back(os.str());
    }
    {
        std::ostringstream os; os << " Ушли:        " << st.left << "  (ТО: " << st.renegedByMaintenance << ")";
        right.push_back(os.str());
    }
    {
        std::ostringstream os; os << std::fixed << std::setprecision(1)
            << " Ожидание:    " << st.avgWaitSeconds << " c"; right.push_back(os.str());
    }
    {
        std::ostringstream os; os << std::fixed << std::setprecision(1)
            << " Обслуж.:     " << st.avgServiceSeconds << " c"; right.push_back(os.str());
    }
    {
        std::ostringstream os; os << std::fixed << std::setprecision(2)
            << " ρ = λ/μ:     " << st.rhoTheoretical
            << (st.rhoTheoretical > 1.0 ? "  (перегруз)" : ""); right.push_back(os.str());
    }
    {
        std::ostringstream os; os << std::fixed << std::setprecision(2)
            << " Загрузка:    " << st.utilization; right.push_back(os.str());
    }
    right.push_back("");
    right.push_back(C(ansi::bold()) + "ПОСЛЕДНИЕ ОПЕРАЦИИ" + R());
    for (int i = 0; i < feedRows; ++i) {
        const int idx = static_cast<int>(ops.size()) - 1 - i;  // новые сверху
        if (idx < 0) { right.push_back(""); continue; }
        const OperationRecord& r = ops[static_cast<std::size_t>(idx)];
        std::ostringstream os;
        os << " #" << r.clientId << ' ' << to_string(r.type);
        const std::string amt = signedAmount(r.type, r.amount);
        if (!amt.empty()) os << ' ' << amt;
        if (r.success) os << "  " << C(ansi::green()) << "OK" << R();
        else os << "  " << C(ansi::red()) << "FAIL " << r.errorMessage << R();
        right.push_back(os.str());
    }

    // === Склейка двух колонок построчно ===
    const std::size_t bodyRows = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < bodyRows; ++i) {
        const std::string lc = (i < left.size()) ? left[i] : std::string{};
        const std::string rc = (i < right.size()) ? right[i] : std::string{};
        L.push_back(fit(lc, leftW) + " " + C(ansi::grey()) + "│" + R() + " " + fit(rc, rightW));
    }

    // === Нижний разделитель (┴) + подсказка команд ===
    L.push_back(C(ansi::grey()) + repeatUtf8("─", leftW + 1) + "┴" +
                repeatUtf8("─", width_ - leftW - 2) + R());
    // Подвал: обычно — подсказка команд; когда все клиенты отработаны — баннер
    // завершения с предложением действий. Обе ветки дают РОВНО одну строку,
    // поэтому высота кадра остаётся постоянной (§4.8.5).
    if (engine_.allClientsProcessed()) {
        L.push_back(fit(C(ansi::green()) + "✓ Симуляция завершена" + R() + C(ansi::grey()) +
                        "  —  restart: новый прогон · stop: выход" + R(), width_));
    } else {
        L.push_back(fit(C(ansi::grey()) +
                        "команды: pause · resume · maintenance N|stop · client N · queue · "
                        "stats · export F · live off · stop" + R(), width_));
    }

    return L;
}

void LiveRenderer::paintFrame() {
    const std::vector<std::string> lines = composeLines();
    // Прячем курсор на ВСЁ время перерисовки. Иначе, пока мы гоняем его по строкам
    // кадра (moveTo на каждую строку), физический курсор терминала прыгал бы по
    // экрану и визуально «мелькал» в разных местах таблицы. Порядок: hideCursor ->
    // сохранить позицию ввода -> нарисовать кадр -> вернуть курсор в позицию ввода
    // -> showCursor. В итоге курсор виден только в строке ввода и не скачет.
    std::string out = ansi::hideCursor();
    out += ansi::saveCursor();  // запомнить, где стоит курсор ввода
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += ansi::moveTo(static_cast<int>(i) + 1, 1);
        out += lines[i];
        out += ansi::clearToLineEnd();
    }
    out += ansi::restoreCursor();
    out += ansi::showCursor();

    std::lock_guard<std::mutex> lock(outMutex_);
    // Повторная проверка ПОД локом. Между «if(!paused_)» в renderLoop и этим
    // моментом мог стартовать overlay: showOverlay() выставляет paused_ и печатает
    // полноэкранный отчёт под ТЕМ ЖЕ outMutex_. Без этой проверки уже собранный
    // кадр дашборда затёр бы отчёт (или врезался бы в него). Если встали на паузу
    // — молча пропускаем кадр.
    if (paused_.load()) return;
    std::cout << out << std::flush;
}

void LiveRenderer::renderLoop() {
    const int hz = (cfg_.ui.refreshHz > 0) ? cfg_.ui.refreshHz : 4;
    const auto period = std::chrono::milliseconds(1000 / hz);
    while (running_.load()) {
        if (!paused_.load()) paintFrame();
        // Прерываемый сон: проснёмся раньше по stop() (running_ станет false).
        std::unique_lock<std::mutex> lock(sleepMutex_);
        sleepCv_.wait_for(lock, period, [this] { return !running_.load(); });
    }
}

void LiveRenderer::start() {
    if (thread_.joinable()) return;  // уже запущен
    running_.store(true);
    thread_ = std::thread([this] { renderLoop(); });
}

void LiveRenderer::stop() {
    running_.store(false);
    sleepCv_.notify_all();  // разбудить render-поток из сна
    if (thread_.joinable()) thread_.join();
}

// Деструктор определён inline в заголовке (см. LiveRenderer.hpp) — здесь его нет.

void LiveRenderer::pause() { paused_.store(true); }
void LiveRenderer::resume() { paused_.store(false); }

}  // namespace atmsim
