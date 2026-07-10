#pragma once
// ============================================================================
//  SceneComposer.hpp — сборка одного кадра сцены на канве (этап 2 feature/scene).
//
//  Разделение обязанностей:
//    * SceneView — «что рисовать»: плоский список актёров с позициями и
//      состояние банкомата. Этап 2 собирает его прямо из снимков движка со
//      статичными слотами (buildSceneView); этап 3 заменит источник актёров
//      на ScenePresenter (твины, походка), а composeScene() не изменится.
//    * composeScene — «как рисовать»: чистая функция SceneView -> канва.
//      Ни движка, ни времени, ни глобальных данных — golden-тесты сравнивают
//      канву с эталоном без терминала.
//
//  Геометрия сцены (namespace layout) вынесена в константы: те же координаты
//  будут целями твинов презентера на этапе 3.
// ============================================================================
#include <optional>
#include <string>
#include <vector>

#include "atmsim/console/scene/SceneCanvas.hpp"
#include "atmsim/console/scene/SceneSprites.hpp"
#include "atmsim/engine/Snapshots.hpp"

namespace atmsim::scene {

// --- Геометрия сцены ---------------------------------------------------------
namespace layout {

constexpr int kAtmX = 1;        // левый край корпуса банкомата
constexpr int kServeX = 19;     // где стоит обслуживаемый клиент (справа от корпуса)
constexpr int kQueueX0 = 27;    // левая колонка первого слота очереди
constexpr int kQueueStep = 7;   // шаг между слотами (3 колонки спрайт + зазор)
constexpr int kActorTopY = 3;   // верхняя строка спрайта человечка (уровень экрана)
constexpr int kLabelY = kActorTopY + 3;  // строка подписи «#id» под человечком
constexpr int kMinSceneRows = 10;        // ниже корпус банкомата (9) + подпись не влезают
constexpr int kMaxSceneRows = 14;

// Левая колонка спрайта для слота очереди index (0 — голова очереди).
constexpr int slotX(int index) { return kQueueX0 + index * kQueueStep; }

// Сколько слотов очереди помещается на канве ширины width: справа резервируем
// ~12 колонок под счётчик переполнения «… ещё NN» и зону прихода.
constexpr int visibleSlots(int width) {
    const int usable = width - 12 - 3 - kQueueX0;
    return usable < 0 ? 0 : usable / kQueueStep + 1;
}

}  // namespace layout

// Один человечек на сцене: где стоит, как выглядит, что подписано снизу.
struct SceneActorView {
    int x = 0;                        // левая колонка спрайта (3 колонки)
    int y = layout::kActorTopY;       // верхняя строка спрайта (дорожка)
    ActorPose pose = ActorPose::Stand;
    Tint tint = Tint::Default;
    bool bold = false;
    std::string label;                // подпись под спрайтом («#12»)
    Tint labelTint = Tint::Grey;
    bool nervous = false;             // терпение на исходе: бейдж «!» над головой
};

// Всё, что нужно нарисовать в одном кадре сцены.
struct SceneView {
    std::vector<SceneActorView> actors;
    // Состояние банкомата (для экрана и индикаторов корпуса).
    AtmState state = AtmState::Idle;
    std::optional<ServiceStage> stage;
    double progress = 0.0;                // доля обслуживания 0..1
    // Клиент взят из очереди, но ещё ИДЁТ к банкомату (walk_seconds): экран
    // показывает «СВОБОДНО», как настоящему подходящему человеку.
    bool approaching = false;
    bool lowCash = false;
    bool maintenancePending = false;
    double maintenanceEtaSeconds = 0.0;   // <0 — ТО до stop, >0 — остаток
    // Хвост очереди, не поместившийся на сцену.
    std::size_t hiddenQueueCount = 0;
    int overflowLabelX = 0;               // колонка счётчика «… ещё N»
    // Спецэффекты (этап 6): включены ли (ui.scene_effects) и фаза анимации
    // 0..7 (задаёт презентер от рендер-часов; в статичной сцене эффектов нет).
    bool effectsEnabled = false;
    int animPhase = 0;
};

// Переносит в SceneView поля состояния банкомата из снимка (экран, индикаторы).
// Общий кусок статичного buildSceneView (этап 2) и ScenePresenter (этап 3).
void fillAtmState(SceneView& view, const AtmSnapshot& atm);

// Маппинг снимков движка в SceneView со статичными позициями слотов (этап 2).
// Используется как запасной путь, пока презентер не сделал ни одного tick
// (первый кадр live-сессии, юнит-тесты композера).
SceneView buildSceneView(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue,
                         int canvasWidth);

// Нарисовать кадр сцены на канве (канва предварительно чиста или будет
// перезаписана: функция сама делает clear()).
void composeScene(const SceneView& view, SceneCanvas& canvas);

}  // namespace atmsim::scene
