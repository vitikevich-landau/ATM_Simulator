#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "atmsim/console/LiveRenderer.hpp"
#include "atmsim/console/Terminal.hpp"
#include "simple_test.hpp"

using namespace atmsim;
using namespace std::chrono_literals;

// Кадр дашборда имеет фиксированную высоту и содержит ключевые подписи.
TEST(renderer_frame_has_labels_and_fixed_height) {
    Config cfg;
    cfg.clients.count = 5;
    cfg.ui.color = false;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg);

    const std::vector<std::string> lines = r.composeLines();
    CHECK(lines.size() > 10);
    CHECK_EQ(static_cast<int>(lines.size()), r.height());

    std::string all;
    for (const auto& l : lines) all += l + '\n';
    CHECK(all.find("ATM Simulator") != std::string::npos);
    CHECK(all.find("Состояние") != std::string::npos);
    CHECK(all.find("Очередь") != std::string::npos);
    CHECK(all.find("Обслужено") != std::string::npos);
}

// Высота кадра НЕ должна зависеть от длины очереди: дашборд рисует фиксированный
// «след» (ровно queueVisible слотов + строка переполнения), иначе плавающая
// высота ломала позицию строки ввода (дублировался хвост команд, затирался ввод).
TEST(renderer_height_stable_regardless_of_queue_length) {
    Config cfg;
    cfg.clients.count = 50;
    cfg.clients.arrivalRatePerMinute = 1e6;   // приходят практически мгновенно
    cfg.simulation.timeScale = 1e6;
    cfg.ui.color = false;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg);

    const int h0 = r.height();                                  // очередь пуста
    CHECK_EQ(static_cast<int>(r.composeLines().size()), h0);

    // Наполняем очередь: без потока обслуживания она только растёт. Поток прихода
    // сам завершится, сгенерировав все 50 клиентов.
    std::thread arr([&] { engine.generateArrivals(); });
    arr.join();

    // Кадр с длинной очередью обязан иметь ТУ ЖЕ высоту, что и при пустой.
    CHECK_EQ(static_cast<int>(r.composeLines().size()), h0);
    engine.requestStop();
}

// При выключенном цвете в кадре не должно быть ANSI-escape (байта ESC = 0x1B).
// Это важно для деградации: в не-TTY выводе не должно быть управляющих кодов.
TEST(renderer_no_ansi_when_color_disabled) {
    Config cfg;
    cfg.clients.count = 5;
    cfg.ui.color = false;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg);

    std::string all;
    for (const auto& l : r.composeLines()) all += l;
    CHECK(all.find('\033') == std::string::npos);
}

// ui.show_progress_bars и ui.events_tail реально влияют на кадр (раньше молча
// игнорировались: полосы всегда, лента жёстко 4 строки).
TEST(renderer_respects_ui_config) {
    // show_progress_bars=false убирает полосы (символы █/░) из кадра.
    {
        Config cfg;
        cfg.clients.count = 5;
        cfg.ui.color = false;
        cfg.ui.showProgressBars = false;
        AtmEngine engine(cfg);
        LiveRenderer r(engine, cfg);
        std::string all;
        for (const auto& l : r.composeLines()) all += l;
        CHECK(all.find("█") == std::string::npos);
        CHECK(all.find("░") == std::string::npos);
    }
    // По умолчанию (show_progress_bars=true) полоса заполнения кассы присутствует.
    {
        Config cfg;
        cfg.clients.count = 5;
        cfg.ui.color = false;  // showProgressBars=true по умолчанию
        AtmEngine engine(cfg);
        LiveRenderer r(engine, cfg);
        std::string all;
        for (const auto& l : r.composeLines()) all += l;
        CHECK(all.find("█") != std::string::npos || all.find("░") != std::string::npos);
    }
    // events_tail управляет длиной ленты: при большом значении правая колонка
    // (лента) выше левой при ЛЮБОЙ высоте терминала (queueVisible <= 47), поэтому
    // высота кадра строго больше, чем при короткой ленте.
    {
        Config few;  few.ui.color = false; few.ui.eventsTail = 2;
        Config many; many.ui.color = false; many.ui.eventsTail = 50;
        AtmEngine e1(few);  LiveRenderer r1(e1, few);
        AtmEngine e2(many); LiveRenderer r2(e2, many);
        CHECK(r2.height() > r1.height());
    }
}

// ТО, запрошенное во время обслуживания, видно на дашборде: строка состояния
// сообщает, что банкомат дорабатывает текущего клиента и после уйдёт на ТО (§4.5).
TEST(renderer_shows_deferred_maintenance) {
    Config cfg;
    cfg.clients.count = 1;
    cfg.clients.arrivalRatePerMinute = 1000.0;              // клиент приходит сразу
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 60.0;   // долгое обслуживание: успеем снять кадр
    cfg.serviceTime.maxSeconds = 60.0;
    cfg.ui.color = false;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().currentClientId.has_value());
    CHECK(engine.requestMaintenance(std::nullopt) == MaintenanceStart::Deferred);

    std::string all;
    for (const auto& l : r.composeLines()) all += l + '\n';
    CHECK(all.find("ТО после текущего клиента") != std::string::npos);

    engine.requestStop();
    eng.join();
    arr.join();
}

// Пока клиент обслуживается, дашборд показывает тематический этап («что делает
// клиент», §4.8) и процент прогресса. Сразу после старта обслуживания этап —
// всегда первый в сценарии: «вставляет карту» (общий для всех операций).
// Заодно закрепляем §4.8.5 для НОВОЙ строки: слот этапа один и тот же и в
// пустом (простой), и в заполненном (обслуживание) состоянии — высота кадра
// не меняется. events_tail = 0, чтобы высоту кадра определяла именно левая
// колонка (при длинной ленте сравнение высот было бы вакуумным).
TEST(renderer_shows_service_stage_while_serving) {
    Config cfg;
    cfg.clients.count = 1;
    cfg.clients.arrivalRatePerMinute = 1000.0;              // клиент приходит сразу
    cfg.clients.patienceSeconds = SecondsRange{1000000, 1000000};
    cfg.serviceTime.distribution = ServiceDistribution::Uniform;
    cfg.serviceTime.minSeconds = 60.0;   // долгое обслуживание: успеем снять кадр
    cfg.serviceTime.maxSeconds = 60.0;
    cfg.ui.color = false;
    cfg.ui.eventsTail = 0;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg);

    const int idleHeight = static_cast<int>(r.composeLines().size());  // никого нет

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline &&
           !engine.snapshot().currentClientId.has_value()) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(engine.snapshot().currentClientId.has_value());

    const std::vector<std::string> lines = r.composeLines();
    std::string all;
    for (const auto& l : lines) all += l + '\n';
    CHECK(all.find("вставляет карту") != std::string::npos);
    CHECK(all.find('%') != std::string::npos);
    CHECK_EQ(static_cast<int>(lines.size()), idleHeight);  // §4.8.5: высота та же

    engine.requestStop();
    eng.join();
    arr.join();
}

// Кламп смещения прокрутки очереди: [0, max(0, total - viewRows)]. Ключевая
// «чистая» логика интерактивного просмотра очереди (листание стрелками).
TEST(terminal_clamp_scroll_offset) {
    // Список короче окна — прокрутки нет, всегда 0.
    CHECK_EQ(clampScrollOffset(0, 3, 10), 0);
    CHECK_EQ(clampScrollOffset(5, 3, 10), 0);
    // Список длиннее окна — максимум total - viewRows.
    CHECK_EQ(clampScrollOffset(0, 100, 10), 0);
    CHECK_EQ(clampScrollOffset(50, 100, 10), 50);
    CHECK_EQ(clampScrollOffset(90, 100, 10), 90);   // ровно на границе
    CHECK_EQ(clampScrollOffset(999, 100, 10), 90);  // не уезжаем за конец
    CHECK_EQ(clampScrollOffset(-5, 100, 10), 0);    // не уезжаем за начало
    // Пустой список.
    CHECK_EQ(clampScrollOffset(3, 0, 10), 0);
}

// Редактирование строки ввода (raw-режим): вставка, ←/→, Home/End, Backspace,
// Delete, Esc, Enter, Eof. Именно эта «чистая» логика лечит артефакты ввода.
TEST(terminal_edit_line) {
    std::string buf;
    std::size_t cur = 0;

    CHECK(editLine(buf, cur, Key::Char, 'a') == LineEdit::Continue);
    editLine(buf, cur, Key::Char, 'b');
    editLine(buf, cur, Key::Char, 'c');
    CHECK_EQ(buf, std::string("abc"));
    CHECK_EQ(cur, static_cast<std::size_t>(3));

    // ← и вставка в середину (главный баг-кейс: вернулся и печатаю).
    editLine(buf, cur, Key::Left, 0);        // cur = 2
    editLine(buf, cur, Key::Char, 'X');      // "abXc", cur = 3
    CHECK_EQ(buf, std::string("abXc"));
    CHECK_EQ(cur, static_cast<std::size_t>(3));

    editLine(buf, cur, Key::Backspace, 0);   // "abc", cur = 2
    CHECK_EQ(buf, std::string("abc"));
    CHECK_EQ(cur, static_cast<std::size_t>(2));

    editLine(buf, cur, Key::Home, 0);        // cur = 0
    CHECK_EQ(cur, static_cast<std::size_t>(0));
    editLine(buf, cur, Key::Delete, 0);      // "bc"
    CHECK_EQ(buf, std::string("bc"));
    CHECK_EQ(cur, static_cast<std::size_t>(0));

    editLine(buf, cur, Key::End, 0);         // cur = 2
    CHECK_EQ(cur, static_cast<std::size_t>(2));

    // Границы не падают: Backspace при cur=0, Delete в конце.
    std::string e; std::size_t ec = 0;
    editLine(e, ec, Key::Backspace, 0);
    CHECK(e.empty());
    editLine(buf, cur, Key::Delete, 0);      // cur в конце — no-op
    CHECK_EQ(buf, std::string("bc"));

    editLine(buf, cur, Key::Escape, 0);      // очистка строки
    CHECK(buf.empty());
    CHECK_EQ(cur, static_cast<std::size_t>(0));

    CHECK(editLine(buf, cur, Key::Enter, 0) == LineEdit::Submit);
    CHECK(editLine(buf, cur, Key::Eof, 0) == LineEdit::Cancel);
}

// displayColumns: колонка курсора считается по символам, а не байтам (кириллица
// = 2 байта). Именно это лечит «уезжающий вправо» курсор при наборе кириллицы.
TEST(terminal_display_columns_utf8) {
    const std::string s = "aфb";  // a=1 байт, ф=2 байта, b=1 байт => 4 байта, 3 колонки
    CHECK_EQ(s.size(), static_cast<std::size_t>(4));
    CHECK_EQ(displayColumns(s, 0), static_cast<std::size_t>(0));
    CHECK_EQ(displayColumns(s, 1), static_cast<std::size_t>(1));  // после 'a'
    CHECK_EQ(displayColumns(s, 3), static_cast<std::size_t>(2));  // после 'ф' (2 байта -> 1 колонка)
    CHECK_EQ(displayColumns(s, 4), static_cast<std::size_t>(3));  // после 'b'
}

// Редактирование кириллицы: ←/→/Backspace работают по целому символу (2 байта),
// а не по одному байту (иначе курсор встал бы в середину символа).
TEST(terminal_edit_line_utf8) {
    const std::string phi = "ф";  // 2 байта UTF-8
    CHECK_EQ(phi.size(), static_cast<std::size_t>(2));

    std::string buf;
    std::size_t cur = 0;
    editLine(buf, cur, Key::Char, phi[0]);  // 'ф' приходит двумя Char-событиями
    editLine(buf, cur, Key::Char, phi[1]);
    editLine(buf, cur, Key::Char, 'x');
    CHECK_EQ(buf, std::string("фx"));
    CHECK_EQ(cur, static_cast<std::size_t>(3));  // 2 + 1 байт

    editLine(buf, cur, Key::Left, 0);            // через 'x' (1 байт)
    CHECK_EQ(cur, static_cast<std::size_t>(2));
    editLine(buf, cur, Key::Left, 0);            // через 'ф' целиком -> 0, не 1
    CHECK_EQ(cur, static_cast<std::size_t>(0));
    editLine(buf, cur, Key::Right, 0);           // снова через 'ф' -> 2
    CHECK_EQ(cur, static_cast<std::size_t>(2));

    editLine(buf, cur, Key::Backspace, 0);       // удаляет весь 'ф' (2 байта)
    CHECK_EQ(buf, std::string("x"));
    CHECK_EQ(cur, static_cast<std::size_t>(0));
}

// Полный жизненный цикл render-потока: старт, перерисовка, пауза/возобновление,
// остановка (в т.ч. повторная) и разрушение. Именно этот путь ловил MSVC как
// «порчу стека вокруг renderer». Здесь проверяем его под ASan/TSan; вывод
// render-потока перехватываем в буфер, чтобы ANSI не сыпался в отчёт тестов.
TEST(renderer_start_stop_lifecycle_is_safe) {
    Config cfg;
    cfg.clients.count = 15;
    cfg.simulation.timeScale = 500.0;
    cfg.ui.color = true;  // путь с ANSI-цветом
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    std::ostringstream sink;
    std::streambuf* old = std::cout.rdbuf(sink.rdbuf());  // перехват до старта потока
    {
        LiveRenderer r(engine, cfg);
        r.start();
        std::this_thread::sleep_for(120ms);
        r.pause();
        std::this_thread::sleep_for(20ms);
        r.resume();
        std::this_thread::sleep_for(60ms);
        r.stop();
        r.stop();  // повторная остановка должна быть безопасной
    }  // деструктор ещё раз останавливает (идемпотентно)
    std::cout.rdbuf(old);  // восстановление после join'а потока

    engine.requestStop();
    eng.join();
    arr.join();

    CHECK(true);  // цель — отсутствие падений/гонок/порчи памяти (ASan/TSan)
}

// То же самое, но с АКТИВНОЙ сценой (принудительный размер терминала 100x40,
// иначе вне TTY сцена не включится): проверяем, что tick презентера в
// render-потоке не вносит гонок и падений (цель — прогоны TSan/ASan).
TEST(renderer_lifecycle_with_scene_is_safe) {
    Config cfg;
    cfg.clients.count = 15;
    cfg.simulation.timeScale = 500.0;
    cfg.ui.color = true;
    cfg.ui.scene = true;
    AtmEngine engine(cfg);

    std::thread eng([&] { engine.run(); });
    std::thread arr([&] { engine.generateArrivals(); });

    std::ostringstream sink;
    std::streambuf* old = std::cout.rdbuf(sink.rdbuf());
    {
        LiveRenderer r(engine, cfg, /*forcedWidth=*/100, /*forcedHeight=*/40);
        r.start();
        std::this_thread::sleep_for(150ms);
        r.pause();
        std::this_thread::sleep_for(20ms);
        r.resume();
        std::this_thread::sleep_for(60ms);
        r.stop();
    }
    std::cout.rdbuf(old);

    engine.requestStop();
    eng.join();
    arr.join();

    CHECK(true);  // отсутствие падений/гонок — проверяют санитайзеры
}
