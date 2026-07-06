#include <string>

#include "atmsim/console/LiveRenderer.hpp"
#include "simple_test.hpp"

using namespace atmsim;

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
