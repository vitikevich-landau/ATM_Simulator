#include "atmsim/console/LiveRenderer.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "atmsim/console/Terminal.hpp"
#include "atmsim/core/Money.hpp"

namespace atmsim {
namespace {

constexpr int kQueueRows = 6;   // сколько строк очереди показываем
constexpr int kFeedRows = 3;    // сколько последних событий показываем
constexpr int kSepWidth = 52;

// Форматирует секунды как HH:MM:SS.
std::string hms(double seconds) {
    long total = static_cast<long>(seconds < 0 ? 0 : seconds);
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

std::string sep() {
    std::string s;
    for (int i = 0; i < kSepWidth; ++i) s += "─";
    return s;
}

}  // namespace

LiveRenderer::LiveRenderer(AtmEngine& engine, const Config& cfg)
    : engine_(engine), cfg_(cfg) {
    // Высота дашборда фиксирована (число строк в кадре одно и то же), считаем
    // её один раз — консоль по ней размещает строку ввода НИЖЕ дашборда.
    height_ = static_cast<int>(composeLines().size());
}

std::vector<std::string> LiveRenderer::composeLines() const {
    const bool color = cfg_.ui.color;
    auto C = [color](const char* code) { return color ? std::string(code) : std::string(); };
    auto R = [color] { return color ? std::string(ansi::reset()) : std::string(); };

    // Три снимка — это три коротких чтения под shared_lock. В v1 допускаем, что
    // панели могут быть из соседних мгновений (расхождение ~мс); для строгой
    // консистентности §4.8.3 рекомендует единый dashboardSnapshot().
    const AtmSnapshot s = engine_.snapshot();
    const std::vector<ClientSnapshot> q = engine_.queueSnapshot();
    OperationFilter feedFilter;
    feedFilter.last = static_cast<std::size_t>(kFeedRows);
    const std::vector<OperationRecord> ops = engine_.operations(feedFilter);

    const std::string cur = cfg_.atm.currency;
    std::vector<std::string> L;

    // 1. Шапка.
    {
        std::ostringstream os;
        os << C(ansi::bold()) << "ATM Simulator" << R()
           << "   uptime " << hms(s.uptimeSeconds)
           << "   [LIVE " << cfg_.ui.refreshHz << " fps]";
        L.push_back(os.str());
    }

    // 2. Состояние + касса (+ остаток ТО).
    {
        const char* sc = ansi::cyan();
        switch (s.state) {
            case AtmState::Serving:     sc = ansi::green();  break;
            case AtmState::Paused:      sc = ansi::yellow(); break;
            case AtmState::Maintenance: sc = ansi::blue();   break;
            case AtmState::Stopped:     sc = ansi::grey();   break;
            case AtmState::Idle:        sc = ansi::grey();   break;
        }
        std::ostringstream os;
        os << "Состояние: " << C(sc) << to_string(s.state) << R();
        if (s.state == AtmState::Maintenance) {
            os << " (";
            if (s.maintenanceEtaSeconds < 0.0) os << "до stop";
            else os << "~" << static_cast<long>(s.maintenanceEtaSeconds) << "c";
            os << ")";
        }
        os << "    Касса: " << (s.lowCash ? C(ansi::red()) : C(ansi::green()))
           << formatMoney(s.cashboxBalance) << R() << ' ' << cur;
        L.push_back(os.str());
    }

    // 3. Полоса кассы.
    {
        const double frac = (cfg_.atm.initialCash > 0)
            ? static_cast<double>(s.cashboxBalance) / static_cast<double>(cfg_.atm.initialCash) : 0.0;
        std::ostringstream os;
        os << "Касса  [" << (s.lowCash ? C(ansi::red()) : C(ansi::green())) << bar(frac, 20) << R() << ']';
        L.push_back(os.str());
    }

    // 4. Разделитель.
    L.push_back(C(ansi::grey()) + sep() + R());

    // 5. Кто обслуживается сейчас.
    {
        std::ostringstream os;
        os << "Обслуживается: ";
        if (s.currentClientId) {
            os << C(ansi::cyan()) << '#' << *s.currentClientId << R() << ' '
               << to_string(*s.currentOperation);
        } else {
            os << C(ansi::grey()) << "—" << R();
        }
        L.push_back(os.str());
    }

    // 6. Разделитель.
    L.push_back(C(ansi::grey()) + sep() + R());

    // 7. Заголовок очереди.
    {
        std::ostringstream os;
        os << "Очередь (" << s.queueLength << ")    макс. за прогон " << s.maxQueueLength;
        L.push_back(os.str());
    }

    // 8..13. Строки очереди (ровно kQueueRows, пустые — пробелами).
    for (int i = 0; i < kQueueRows; ++i) {
        if (i < static_cast<int>(q.size())) {
            const ClientSnapshot& c = q[static_cast<std::size_t>(i)];
            const double total = c.waitedSeconds + c.remainingPatience;
            const double frac = (total > 0.0) ? c.remainingPatience / total : 0.0;
            const char* pc = (frac > 0.4) ? ansi::green() : (frac > 0.15) ? ansi::yellow() : ansi::red();
            std::ostringstream os;
            os << "  #" << c.id << ' ' << to_string(c.requestedOperation);
            if (c.requestedOperation != OperationType::CheckBalance) os << ' ' << formatMoney(c.amount);
            os << "   ждёт " << static_cast<long>(c.waitedSeconds) << "c   ["
               << C(pc) << bar(frac, 8) << R() << "] " << static_cast<long>(c.remainingPatience) << "c";
            L.push_back(os.str());
        } else {
            L.push_back(" ");
        }
    }

    // 14. Индикатор «ещё N».
    if (q.size() > static_cast<std::size_t>(kQueueRows)) {
        L.push_back(C(ansi::grey()) + "  … ещё " + std::to_string(q.size() - kQueueRows) + R());
    } else {
        L.push_back(" ");
    }

    // 15. Разделитель.
    L.push_back(C(ansi::grey()) + sep() + R());

    // 16. Счётчики.
    {
        std::ostringstream os;
        os << "Обслужено " << s.totalServed << "    Ушли " << s.totalLeft;
        L.push_back(os.str());
    }

    // 17. Заголовок ленты.
    L.push_back("Последние события:");

    // 18..20. Лента (новые сверху; ровно kFeedRows строк).
    for (int i = 0; i < kFeedRows; ++i) {
        const int idx = static_cast<int>(ops.size()) - 1 - i;  // с конца (новые)
        if (idx >= 0) {
            const OperationRecord& r = ops[static_cast<std::size_t>(idx)];
            std::ostringstream os;
            os << "  #" << r.clientId << ' ' << to_string(r.type);
            if (r.type != OperationType::CheckBalance) os << ' ' << formatMoney(r.amount);
            if (r.success) os << ' ' << C(ansi::green()) << "OK" << R();
            else os << ' ' << C(ansi::red()) << "FAIL " << r.errorMessage << R();
            L.push_back(os.str());
        } else {
            L.push_back(" ");
        }
    }

    return L;
}

void LiveRenderer::paintFrame() {
    const std::vector<std::string> lines = composeLines();
    // Собираем весь кадр в одну строку и печатаем атомарно под локом вывода.
    std::string out = ansi::saveCursor();  // запомнить, где стоит курсор ввода
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += ansi::moveTo(static_cast<int>(i) + 1, 1);  // абсолютная позиция строки
        out += lines[i];
        out += ansi::clearToLineEnd();  // затереть «хвост» прошлого кадра
    }
    out += ansi::restoreCursor();  // вернуть курсор на строку ввода — её мы не трогали

    std::lock_guard<std::mutex> lock(outMutex_);
    std::cout << out << std::flush;
}

void LiveRenderer::renderLoop(std::stop_token stopToken) {
    const int hz = (cfg_.ui.refreshHz > 0) ? cfg_.ui.refreshHz : 4;
    const auto period = std::chrono::milliseconds(1000 / hz);
    while (!stopToken.stop_requested()) {
        if (!paused_.load()) paintFrame();
        // Прерываемый сон: проснёмся раньше по запросу остановки (stop_token).
        std::unique_lock<std::mutex> lock(sleepMutex_);
        sleepCv_.wait_for(lock, stopToken, period, [] { return false; });
    }
}

void LiveRenderer::start() {
    thread_ = std::jthread([this](std::stop_token st) { renderLoop(st); });
}

void LiveRenderer::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        sleepCv_.notify_all();
        thread_.join();
    }
}

void LiveRenderer::pause() { paused_.store(true); }
void LiveRenderer::resume() { paused_.store(false); }

}  // namespace atmsim
