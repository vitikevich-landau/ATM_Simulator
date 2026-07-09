#include "atmsim/console/LiveRenderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "atmsim/console/Terminal.hpp"
#include "atmsim/console/scene/SceneCanvas.hpp"
#include "atmsim/console/scene/SceneComposer.hpp"
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

LiveRenderer::LiveRenderer(AtmEngine& engine, const Config& cfg, int forcedWidth,
                           int forcedHeight)
    : engine_(engine), cfg_(cfg) {
    // Размеры терминала — один раз при создании (ресайз на лету — v2).
    // Принудительные размеры (>0) — только для тестов, см. LiveRenderer.hpp.
    width_ = std::clamp(forcedWidth > 0 ? forcedWidth : Terminal::width(), 60, 200);
    termHeight_ = std::clamp(forcedHeight > 0 ? forcedHeight : Terminal::height(), 16, 60);

    // Сцена включается, только если она СО ВСЕЙ таблицей помещается в терминал.
    // Бюджет тела (колонки очереди/статистики) в scene-режиме: высота терминала
    // минус шапка (2), три разделителя (3), подвал (1), строка ввода с запасом
    // (2) и сама сцена. Минимум 12 строк тела = очередь 4+5 служебных ИЛИ
    // лента 3+9 служебных — меньше таблица теряет смысл. Не влезло — молча
    // остаёмся в таблице: высота кадра постоянна в обоих случаях (§4.8.5).
    sceneRows_ = std::clamp(cfg_.ui.sceneRows, scene::layout::kMinSceneRows,
                            scene::layout::kMaxSceneRows);
    const int bodyBudget = termHeight_ - 8 - sceneRows_;
    sceneActive_ = cfg_.ui.scene && width_ >= 84 && bodyBudget >= 12;
    if (sceneActive_) {
        presenter_ = std::make_unique<scene::ScenePresenter>(width_, sceneRows_);
    }

    height_ = static_cast<int>(composeLines().size());
}

std::vector<std::string> LiveRenderer::composeLines() const {
    // Публичный вариант (тесты, первый расчёт высоты): снимаем свежие снимки
    // и делегируем сборку. Render-цикл сюда не ходит — у него кэш.
    OperationFilter feedFilter;
    feedFilter.last = static_cast<std::size_t>(std::clamp(cfg_.ui.eventsTail, 0, 50));
    return composeLinesFrom(engine_.snapshot(), engine_.statsSnapshot(),
                            engine_.queueSnapshot(), engine_.operations(feedFilter));
}

std::vector<std::string> LiveRenderer::composeLinesFrom(
    const AtmSnapshot& s, const StatsSnapshot& st, const std::vector<ClientSnapshot>& q,
    const std::vector<OperationRecord>& ops) const {
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

    // Длина ленты последних операций — из конфига (ui.events_tail), с разумным
    // потолком, чтобы битый конфиг не растянул кадр до абсурда. Значение постоянно
    // в пределах запуска, поэтому высота кадра остаётся стабильной (§4.8.5).
    // В scene-режиме лента дополнительно ужимается под остаток высоты после
    // сценической полосы (9 = строки статистики над лентой), но не короче 3.
    // ops может содержать БОЛЬШЕ записей, чем нужно ленте (кэш render-цикла
    // берёт с запасом для судеб сцены) — лента показывает feedRows новейших.
    const int sceneBodyBudget = termHeight_ - 8 - sceneRows_;
    const int feedRows = sceneActive_
        ? std::clamp(std::min(cfg_.ui.eventsTail, sceneBodyBudget - 9), 3, 50)
        : std::clamp(cfg_.ui.eventsTail, 0, 50);

    const std::string cur = cfg_.atm.currency;

    // Геометрия: левая колонка (очередь) шире, правая — статистика/лента.
    const int leftW = std::clamp(width_ - 42, 40, 54);
    const int rightW = width_ - leftW - 3;              // 3 = " │ "
    // Число слотов очереди: без сцены — вся высота терминала минус служебные
    // строки; со сценой — остаток бюджета тела (5 = строки левой колонки
    // над слотами и под ними). Оба значения постоянны в пределах сессии.
    const int queueVisible = sceneActive_ ? std::clamp(sceneBodyBudget - 5, 4, 99)
                                          : std::max(4, termHeight_ - 13);

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
        } else if (s.maintenancePending) {
            // ТО запрошено, но текущий клиент дорабатывает (§4.5): показываем,
            // что банкомат уже «уходит на ТО», просто не бросает операцию.
            os << ' ' << C(ansi::blue()) << "(→ ТО после текущего клиента)" << R();
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

    // === Сценическая полоса (feature/scene): банкомат и человечки ===
    // Полоса живёт МЕЖДУ шапкой и таблицей: сцена показывает «как это выглядит»,
    // таблица ниже — точные цифры (решение владельца: цифры не жертвуем).
    // Высота полосы постоянна (sceneRows_), поэтому §4.8.5 не нарушается.
    if (sceneActive_) {
        L.push_back(C(ansi::grey()) + repeatUtf8("─", width_) + R());
        scene::SceneCanvas canvas(width_, sceneRows_);
        // Живые актёры презентера (двигаются между кадрами); пока он не сделал
        // ни одного tick (первый кадр, тесты без tick) — статичная сцена по
        // текущему снимку.
        if (presenter_ && presenter_->hasTicked()) {
            scene::composeScene(presenter_->view(), canvas);
        } else {
            scene::composeScene(scene::buildSceneView(s, q, width_), canvas);
        }
        for (std::string& line : canvas.toLines(color)) L.push_back(std::move(line));
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
    // Строка «что делает клиент» (§4.8): тематический этап обслуживания и доля
    // отработанного времени. Пока никого не обслуживают, строка пуста — она
    // ВСЕГДА занимает ровно один слот, чтобы высота кадра не плавала (§4.8.5).
    {
        std::ostringstream os;
        if (s.currentClientId && s.currentStage) {
            os << " └ " << C(ansi::cyan()) << to_string(*s.currentStage) << R() << ' ';
            // Полоса прогресса обслуживания — как и остальные полосы, только
            // при ui.show_progress_bars; процент показываем всегда.
            if (cfg_.ui.showProgressBars) {
                os << '[' << C(ansi::cyan()) << bar(s.serviceProgress, 10) << R() << "] ";
            }
            os << static_cast<int>(std::lround(s.serviceProgress * 100.0)) << '%';
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

void LiveRenderer::paintFrame(const std::vector<std::string>& lines) {
    // Дифф: в терминал уходят только изменившиеся строки (этап 5). Кадр без
    // изменений не трогает терминал вовсе (и не дёргает outMutex_ — ввод
    // пользователя не конкурирует с пустыми перерисовками).
    const std::string body = differ_.diff(lines);
    if (body.empty()) return;

    // Прячем курсор на время перерисовки (иначе, гоняя его по строкам кадра через
    // moveTo, мы бы видели его «пробег» по таблице). Рисуем дашборд, затем ЯВНО
    // ставим курсор в позицию ВВОДА (её сообщает консоль через setCursorTarget) и
    // показываем. Явная установка, а не save/restore, важна на переходах: после
    // clearScreen (старт live-режима, закрытие отчёта/очереди) сохранённая позиция
    // была бы (1,1) — курсор «прыгал» в таблицу, пока не перерисуется строка ввода.
    // Кадр обёрнут в synchronized output (DEC 2026): Windows Terminal показывает
    // его атомарно, без полусобранных состояний; conhost молча игнорирует.
    std::string out = ansi::syncBegin();
    out += ansi::hideCursor();
    out += body;
    out += ansi::moveTo(cursorRow_.load(), cursorCol_.load());
    out += ansi::showCursor();
    out += ansi::syncEnd();

    std::lock_guard<std::mutex> lock(outMutex_);
    // Повторная проверка ПОД локом. Между «if(!paused_)» в renderLoop и этим
    // моментом мог стартовать overlay: showOverlay() выставляет paused_ и печатает
    // полноэкранный отчёт под ТЕМ ЖЕ outMutex_. Без этой проверки уже собранный
    // кадр дашборда затёр бы отчёт (или врезался бы в него). Если встали на паузу
    // — пропускаем кадр И инвалидируем дифф: differ_ уже записал кадр как
    // «отрисованный», хотя терминал его не получил.
    if (paused_.load()) {
        differ_.invalidate();
        return;
    }
    std::cout << out << std::flush;
}

void LiveRenderer::renderLoop() {
    using clock = std::chrono::steady_clock;
    // Разрешение таймера Windows -> 1 мс на время live-режима: иначе дедлайны
    // кадров на 15 fps дрожат на штатной гранулярности ~15.6 мс (этап 0).
    TimerResolutionGuard timerGuard;

    // Частоты РАЗДЕЛЕНЫ: снимки движка опрашиваются на refresh_hz (не грузим
    // его мьютексы чаще, чем осмысленно меняются данные), кадры рисуются на
    // scene_fps из кэша — анимации между снимками остаются плавными. Без
    // сцены кадры и снимки идут на одной частоте, как раньше.
    const int hz = std::clamp(cfg_.ui.refreshHz, 1, 60);
    const int fps = sceneActive_ ? std::clamp(cfg_.ui.sceneFps, 5, 30) : hz;
    const auto framePeriod = std::chrono::microseconds(1'000'000 / fps);
    const auto snapPeriod = std::chrono::microseconds(1'000'000 / hz);

    const auto start = clock::now();
    long long frame = 0;
    auto lastSnap = start - snapPeriod;  // первый кадр всегда со свежими снимками

    while (running_.load()) {
        if (!paused_.load()) {
            // Заявка от resume(): полный repaint (overlay затёр экран) и
            // мгновенная расстановка актёров. Выполняется ЗДЕСЬ, в render-
            // потоке — differ_ и presenter_ не потокобезопасны.
            if (teleportOnResume_.exchange(false)) {
                differ_.invalidate();
                if (presenter_) presenter_->requestTeleport();
            }

            const auto nowTp = clock::now();
            if (nowTp - lastSnap >= snapPeriod) {
                cachedSnap_ = engine_.snapshot();
                cachedStats_ = engine_.statsSnapshot();
                cachedQueue_ = engine_.queueSnapshot();
                // Один хвост ленты и для таблицы, и для судеб сцены: берём с
                // запасом (не меньше 12 записей на грейс-логику презентера).
                OperationFilter f;
                f.last = static_cast<std::size_t>(
                    std::max(std::clamp(cfg_.ui.eventsTail, 0, 50), 12));
                cachedOps_ = engine_.operations(f);
                lastSnap = nowTp;
            }

            if (presenter_) {
                const double nowSec =
                    std::chrono::duration<double>(nowTp.time_since_epoch()).count();
                presenter_->tick(cachedSnap_, cachedQueue_, nowSec, cachedOps_);
            }
            paintFrame(composeLinesFrom(cachedSnap_, cachedStats_, cachedQueue_, cachedOps_));
        }

        // Дедлайновый пейсинг: следующий кадр строго в start + N*period (а не
        // «сон на номинал после работы» — тот накапливает дрейф). Опоздали
        // больше чем на кадр — не гонимся за прошедшими дедлайнами (drop-frame),
        // а перепланируемся от текущего момента.
        ++frame;
        auto deadline = start + framePeriod * frame;
        const auto lateBy = clock::now() - deadline;
        if (lateBy > framePeriod) {
            frame += lateBy / framePeriod;
            deadline = start + framePeriod * frame;
        }
        // Прерываемое ожидание: проснёмся раньше по stop() (running_ = false).
        std::unique_lock<std::mutex> lock(sleepMutex_);
        sleepCv_.wait_until(lock, deadline, [this] { return !running_.load(); });
    }
}

void LiveRenderer::start() {
    if (thread_.joinable()) return;  // уже запущен
    // Экран мог быть очищен/перерисован между сессиями start/stop — первый
    // кадр нового запуска обязан быть полным.
    differ_.invalidate();
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

void LiveRenderer::resume() {
    // Overlay затёр экран, а за время паузы состояние могло уехать далеко:
    // просим render-поток сделать полный repaint и расставить актёров сцены
    // мгновенно (флаг — потому что differ_/presenter_ принадлежат ему).
    teleportOnResume_.store(true);
    paused_.store(false);
}

}  // namespace atmsim
