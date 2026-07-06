#include <chrono>
#include <iostream>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>

#include "atmsim/console/LiveRenderer.hpp"
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

    std::jthread eng([&](std::stop_token st) { engine.run(st); });
    std::jthread arr([&](std::stop_token st) { engine.generateArrivals(st); });

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
    eng.request_stop();
    arr.request_stop();
    eng.join();
    arr.join();

    CHECK(true);  // цель — отсутствие падений/гонок/порчи памяти (ASan/TSan)
}
