// ============================================================================
//  test_scene_compose.cpp — тесты статичной сцены (этап 2 feature/scene):
//  спрайты, сборка SceneView из снимков, отрисовка, интеграция в LiveRenderer
//  и новые ключи конфигурации.
//
//  Сцена через LiveRenderer проверяется с ПРИНУДИТЕЛЬНЫМ размером терминала
//  (вне TTY реальные размеры откатываются в 80x24, где сцена не включается).
// ============================================================================
#include <string>
#include <thread>
#include <vector>

#include "atmsim/config/ConfigLoader.hpp"
#include "atmsim/console/LiveRenderer.hpp"
#include "atmsim/console/scene/GlyphSet.hpp"
#include "atmsim/console/scene/SceneComposer.hpp"
#include "atmsim/console/scene/SceneSprites.hpp"
#include "simple_test.hpp"

using namespace atmsim;
using scene::ActorPose;
using scene::SceneCanvas;
using scene::SceneView;

namespace {

// Склейка канвы в один текст для поиска подстрок.
std::string flatten(const SceneCanvas& canvas) {
    std::string all;
    for (const std::string& l : canvas.toLines(false)) all += l + '\n';
    return all;
}

// Склейка кадра рендерера.
std::string flatten(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& l : lines) all += l + '\n';
    return all;
}

}  // namespace

// Все глифы художественных ресурсов обязаны входить в белый список GlyphSet —
// иначе в release они превратятся в '?', а в debug уронят assert при blit.
TEST(scene_sprites_use_only_allowed_glyphs) {
    auto checkRows = [](const std::vector<std::string>& rows) {
        for (const std::string& row : rows) {
            for (char32_t cp : scene::utf8::decode(row)) {
                CHECK(scene::isAllowedGlyph(cp));
            }
        }
    };
    checkRows(scene::poseRows(ActorPose::Stand));
    checkRows(scene::poseRows(ActorPose::ReachLeft));
    checkRows(scene::poseRows(ActorPose::WalkA));
    checkRows(scene::poseRows(ActorPose::WalkB));
    checkRows(scene::atmArtRows());
    // Корпус банкомата — ровно заявленные размеры (спрайт непрозрачный).
    CHECK_EQ(scene::atmArtRows().size(), static_cast<std::size_t>(scene::kAtmArtRows));
    for (const std::string& row : scene::atmArtRows()) {
        CHECK_EQ(scene::utf8::decode(row).size(), static_cast<std::size_t>(scene::kAtmArtWidth));
    }
    // Реплики экрана: только разрешённые глифы и не шире экрана (12 колонок).
    const ServiceStage all[] = {
        ServiceStage::InsertCard,   ServiceStage::EnterPin,    ServiceStage::SelectOperation,
        ServiceStage::EnterAmount,  ServiceStage::BankRequest, ServiceStage::CountCash,
        ServiceStage::DispenseCash, ServiceStage::InsertCash,  ServiceStage::VerifyCash,
        ServiceStage::ShowBalance,  ServiceStage::PrintReceipt, ServiceStage::ReturnCard,
    };
    for (ServiceStage st : all) {
        for (const std::string& line : scene::screenStageText(st)) {
            const std::u32string cps = scene::utf8::decode(line);
            CHECK(cps.size() <= static_cast<std::size_t>(scene::kAtmScreenWidth));
            for (char32_t cp : cps) CHECK(scene::isAllowedGlyph(cp));
        }
    }
}

// Сцена обслуживания: банкомат с репликой этапа, обслуживаемый у корпуса,
// очередь в слотах, скрытый хвост — в счётчике «… ещё N».
TEST(scene_compose_serving_with_queue_and_overflow) {
    AtmSnapshot atm;
    atm.state = AtmState::Serving;
    atm.currentClientId = 7;
    atm.currentOperation = OperationType::Withdraw;
    atm.currentStage = ServiceStage::EnterPin;
    atm.serviceProgress = 0.34;

    std::vector<ClientSnapshot> queue;
    for (int i = 0; i < 12; ++i) {
        ClientSnapshot c;
        c.id = static_cast<ClientId>(100 + i);
        queue.push_back(c);
    }

    const int width = 100;
    const SceneView view = scene::buildSceneView(atm, queue, width);
    // На 100 колонках помещается 9 слотов (см. layout::visibleSlots) — трое в хвосте.
    CHECK_EQ(static_cast<int>(view.actors.size()), 1 + scene::layout::visibleSlots(width));
    CHECK_EQ(view.hiddenQueueCount, std::size_t{12 - 9});

    SceneCanvas canvas(width, 10);
    scene::composeScene(view, canvas);
    const std::string all = flatten(canvas);
    CHECK(all.find("БАНКОМАТ") != std::string::npos);
    CHECK(all.find("ВВЕДИТЕ") != std::string::npos);      // реплика EnterPin
    CHECK(all.find("PIN-КОД") != std::string::npos);
    CHECK(all.find("34%") != std::string::npos);          // прогресс на экране
    CHECK(all.find("#7") != std::string::npos);           // обслуживаемый
    CHECK(all.find("#100") != std::string::npos);         // голова очереди
    CHECK(all.find("… ещё 3") != std::string::npos);      // скрытый хвост
    // Каждая строка канвы — ровно width кодовых точек (инвариант раскладки).
    for (const std::string& l : canvas.toLines(false)) {
        CHECK_EQ(scene::utf8::decode(l).size(), static_cast<std::size_t>(width));
    }
}

// Экран банкомата отражает состояния: простой, пауза, ТО (с остатком и «до
// команды»), останов; «→ ТЕХРАБОТЫ» на шильдике при отложенном ТО.
TEST(scene_compose_screens_by_state) {
    std::vector<ClientSnapshot> empty;
    SceneCanvas canvas(90, 10);

    AtmSnapshot idle;
    idle.state = AtmState::Idle;
    scene::composeScene(scene::buildSceneView(idle, empty, 90), canvas);
    CHECK(flatten(canvas).find("СВОБОДНО") != std::string::npos);

    AtmSnapshot paused;
    paused.state = AtmState::Paused;
    scene::composeScene(scene::buildSceneView(paused, empty, 90), canvas);
    CHECK(flatten(canvas).find("ПАУЗА") != std::string::npos);

    AtmSnapshot mnt;
    mnt.state = AtmState::Maintenance;
    mnt.maintenanceEtaSeconds = 42.0;
    scene::composeScene(scene::buildSceneView(mnt, empty, 90), canvas);
    CHECK(flatten(canvas).find("ТЕХРАБОТЫ") != std::string::npos);
    CHECK(flatten(canvas).find("~42с") != std::string::npos);

    mnt.maintenanceEtaSeconds = -1.0;  // ТО до явной команды stop
    scene::composeScene(scene::buildSceneView(mnt, empty, 90), canvas);
    CHECK(flatten(canvas).find("до команды") != std::string::npos);

    AtmSnapshot pending;
    pending.state = AtmState::Serving;
    pending.currentClientId = 3;
    pending.maintenancePending = true;
    scene::composeScene(scene::buildSceneView(pending, empty, 90), canvas);
    CHECK(flatten(canvas).find("→ ТЕХРАБОТЫ") != std::string::npos);
}

// Интеграция: при ui.scene=true и большом терминале кадр содержит сцену,
// высота кадра стабильна и не выходит за пределы терминала.
TEST(scene_in_renderer_frame_when_enabled_and_fits) {
    Config cfg;
    cfg.clients.count = 5;
    cfg.ui.color = false;
    cfg.ui.scene = true;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg, /*forcedWidth=*/100, /*forcedHeight=*/40);

    const std::vector<std::string> lines = r.composeLines();
    const std::string all = flatten(lines);
    CHECK(all.find("БАНКОМАТ") != std::string::npos);   // сцена есть
    CHECK(all.find("СТАТИСТИКА") != std::string::npos); // таблица на месте
    CHECK(all.find("Очередь") != std::string::npos);
    CHECK_EQ(static_cast<int>(lines.size()), r.height());
    // Кадр + строка ввода (height+2) обязаны помещаться в терминал.
    CHECK(r.height() + 2 <= 40);
    // Нет цвета — нет ESC (сцена уважает ui.color, как и таблица).
    CHECK(all.find('\033') == std::string::npos);
}

// Терминал мал (запасные 80x24 вне TTY) — сцена молча выключается, кадр
// идентичен обычному табличному.
TEST(scene_falls_back_to_table_on_small_terminal) {
    Config cfg;
    cfg.clients.count = 5;
    cfg.ui.color = false;

    Config cfgScene = cfg;
    cfgScene.ui.scene = true;

    AtmEngine engine(cfg);
    LiveRenderer plain(engine, cfg, 80, 24);
    LiveRenderer scened(engine, cfgScene, 80, 24);
    CHECK_EQ(plain.height(), scened.height());
    CHECK(flatten(scened.composeLines()).find("БАНКОМАТ") == std::string::npos);
}

// Высота кадра со сценой не зависит от длины очереди (§4.8.5) — слоты сцены
// и слоты таблицы рисуют фиксированный след.
TEST(scene_frame_height_stable_with_queue) {
    Config cfg;
    cfg.clients.count = 50;
    cfg.clients.arrivalRatePerMinute = 1e6;
    cfg.simulation.timeScale = 1e6;
    cfg.ui.color = false;
    cfg.ui.scene = true;
    AtmEngine engine(cfg);
    LiveRenderer r(engine, cfg, 100, 40);

    const int h0 = r.height();
    std::thread arr([&] { engine.generateArrivals(); });
    arr.join();
    CHECK_EQ(static_cast<int>(r.composeLines().size()), h0);
    engine.requestStop();
}

// Новые ключи конфига: чтение и границы.
TEST(scene_config_keys_parse_and_validate) {
    const Config c = ConfigLoader::loadFromString(
        R"({"ui": {"scene": true, "scene_fps": 20, "scene_rows": 12}})");
    CHECK(c.ui.scene);
    CHECK_EQ(c.ui.sceneFps, 20);
    CHECK_EQ(c.ui.sceneRows, 12);

    // Значения по умолчанию: сцена выключена.
    const Config d = ConfigLoader::loadFromString("{}");
    CHECK(!d.ui.scene);
    CHECK_EQ(d.ui.sceneFps, 15);
    CHECK_EQ(d.ui.sceneRows, 10);

    // Выход за границы — осмысленная ошибка конфигурации, не UB глубоко в рендере.
    bool threw = false;
    try {
        ConfigLoader::loadFromString(R"({"ui": {"scene_fps": 100}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        ConfigLoader::loadFromString(R"({"ui": {"scene_rows": 5}})");
    } catch (const ConfigError&) {
        threw = true;
    }
    CHECK(threw);
}

// Слой эффектов (этап 6): спиннер связи с банком, купюры у лотка; всё
// отключается флагом effectsEnabled (ui.scene_effects).
TEST(scene_compose_effects_layer) {
    using scene::utf8::decode;
    AtmSnapshot atm;
    atm.state = AtmState::Serving;
    atm.currentClientId = 1;
    atm.currentOperation = OperationType::Withdraw;
    atm.currentStage = ServiceStage::BankRequest;
    atm.serviceProgress = 0.5;

    SceneView v = scene::buildSceneView(atm, {}, 90);
    v.effectsEnabled = true;
    v.animPhase = 1;  // фаза 1 -> кадр спиннера '/'
    SceneCanvas canvas(90, 10);
    scene::composeScene(v, canvas);
    // Спиннер живёт в правом углу экрана банкомата: колонка screenX+11 (=14).
    const std::u32string screenRow = decode(canvas.toLines(false)[3]);
    CHECK(screenRow[14] == U'/');

    // Выдача наличных: две «купюры» (=) на дорожке лотка (строка 7).
    atm.currentStage = ServiceStage::DispenseCash;
    v = scene::buildSceneView(atm, {}, 90);
    v.effectsEnabled = true;
    v.animPhase = 0;
    scene::composeScene(v, canvas);
    const std::u32string trayRow = decode(canvas.toLines(false)[7]);
    int bills = 0;
    for (int x = 15; x < scene::layout::kServeX; ++x) {
        if (trayRow[static_cast<std::size_t>(x)] == U'=') ++bills;
    }
    CHECK_EQ(bills, 2);

    // Флаг выключен — дорожка чиста, спиннера нет.
    v.effectsEnabled = false;
    scene::composeScene(v, canvas);
    const std::u32string quietRow = decode(canvas.toLines(false)[7]);
    for (int x = 15; x < scene::layout::kServeX; ++x) {
        CHECK(quietRow[static_cast<std::size_t>(x)] != U'=');
    }
}
