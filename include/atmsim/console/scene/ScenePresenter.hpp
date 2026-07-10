#pragma once
// ============================================================================
//  ScenePresenter.hpp — презентационный слой актёров (этап 3 feature/scene).
//
//  ПРОБЛЕМА, которую решает этот класс: снимки движка — дискретные состояния
//  («кто в очереди сейчас»), а анимация требует непрерывности («человечек ИДЁТ
//  к своему месту»). Presenter держит собственный реестр актёров по ClientId
//  (позиция float, твин движения, стадия жизни) и каждый кадр СОГЛАСУЕТ его
//  с очередным снимком — как client-side reconciliation в сетевых играх:
//  снимок авторитетен («кто где логически»), актёры лишь плавно догоняют его.
//
//  Правила согласования (дифф множеств, а не событий — события, случившиеся
//  между кадрами, коалесцируются сами):
//    * id появился в снимке     -> актёр входит с правого края и идёт к цели
//                                  (walk-in); «появился и исчез между тиками»
//                                  не спавнится вовсе (норма при time_scale>>1);
//    * у актёра сменилась цель  -> новый твин стартует ИЗ ТЕКУЩЕЙ точки
//                                  (без телепорта), скорость с catch-up;
//                                  при дистанции больше полусцены — телепорт:
//                                  снимок авторитетен, красота вторична;
//    * id исчез из снимка       -> актёр доигрывает уход (обслуженный —
//                                  довольный, из очереди — потерял терпение)
//                                  по нижней «дорожке выхода» и исчезает за
//                                  краем; в снимке его уже нет — уходящие
//                                  живут только здесь. Одновременных уходящих
//                                  не больше kMaxLeavers (старейший вытесняется);
//    * хвост очереди за пределами видимых слотов НЕ спавнится (виртуализация,
//      счётчик «… ещё N»); вход в видимую зону = обычный walk-in.
//
//  ВРЕМЯ подаётся снаружи параметром nowSec (монотонные секунды): рендер-поток
//  передаёт steady_clock, тесты — любые значения. Никаких обращений к часам
//  внутри — поведение детерминировано и покрывается тестами побайтно.
//
//  ВЛАДЕНИЕ И ПОТОКИ: presenter принадлежит render-потоку LiveRenderer
//  (tick() зовётся из renderLoop перед отрисовкой кадра). Он НИКОГДА не
//  трогает движок — только копии-снимки. Извне (тесты) tick()/view() зовутся
//  строго однопоточно.
// ============================================================================
#include <map>
#include <vector>

#include "atmsim/console/scene/SceneComposer.hpp"  // SceneView, layout
#include "atmsim/engine/Snapshots.hpp"

namespace atmsim::scene {

class ScenePresenter {
public:
    // canvasWidth/sceneRows — размеры сценической полосы (фиксированы на
    // всю live-сессию, как и у LiveRenderer); effects — ui.scene_effects
    // (спиннер связи с банком, купюры, «пар» злости); timeScale —
    // cfg.simulation.time_scale: переводит модельные секунды ПОДХОДА из
    // снимка в реальные секунды твина (подход — единственное движение сцены,
    // темп которого задаёт движок, а не константы презентера).
    explicit ScenePresenter(int canvasWidth, int sceneRows, bool effects = true,
                            double timeScale = 1.0);

    // Согласовать реестр актёров со снимками движка и продвинуть анимации к
    // моменту nowSec. Зовётся один раз на кадр ПЕРЕД composeLines().
    // opsTail — хвост ленты операций: по нему определяется СУДЬБА исчезнувшего
    // из снимка клиента (операция OK -> ушёл довольным, FAIL -> растерянным,
    // записи нет -> не дождался). Пустой хвост допустим (тесты этапа 3).
    void tick(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue, double nowSec,
              const std::vector<OperationRecord>& opsTail = {});

    // Был ли хотя бы один tick (до него view() пуста, а composeLines()
    // откатывается на статичную сцену buildSceneView).
    bool hasTicked() const { return ticked_; }

    // Кадр сцены по состоянию на последний tick().
    const SceneView& view() const { return view_; }

    // Следующий tick() расставит актёров по снимку МГНОВЕННО, без анимаций
    // входа/переходов: старт live-сессии, возврат после overlay-отчёта (этап 5),
    // когда проигрывать «догоняющие» анимации за время паузы бессмысленно.
    void requestTeleport() { teleportNext_ = true; }

private:
    // Стадия жизни актёра на сцене (не путать с ClientState движка: это чисто
    // презентационное состояние, движок о нём не знает).
    enum class ActorState {
        WalkIn,        // входит с правого края к своему месту
        QueueIdle,     // стоит в очереди
        Advance,       // продвигается к новому слоту / к банкомату
        AtAtm,         // у банкомата (обслуживается)
        LeavePending,  // исчез из снимка; ждём записи в ленте (грейс 2 тика)
        LeaveHappy,    // уходит обслуженный (операция OK)
        LeavePuzzled,  // уходит растерянный (операция FAIL)
        LeaveAngry,    // уходит, не дождавшись (записи в ленте нет)
    };

    // Твин позиции: плавный ход fromX -> toX за [start, start+dur].
    struct Tween {
        double fromX = 0.0;
        double toX = 0.0;
        double start = 0.0;
        double dur = 0.0;  // 0 -> позиция уже toX
    };

    struct Actor {
        ClientId id{};
        ActorState state = ActorState::QueueIdle;
        Tween tween;
        int y = layout::kActorTopY;   // строка спрайта (дорожка)
        bool serving = false;          // текущая роль по последнему снимку
        double stateSince = 0.0;       // когда вошёл в текущую стадию (для cap)
        int graceTicks = 0;            // остаток грейс-периода LeavePending
    };

    static double posAt(const Tween& t, double now);
    // Перенаправить актёра к цели targetX: новый твин из ТЕКУЩЕЙ точки с
    // catch-up скоростью; телепорт при дистанции больше полусцены.
    void retarget(Actor& a, double targetX, double now) const;
    // Ведение ПОДХОДА к банкомату (walk_seconds): цель и срок задаёт снимок
    // движка (remainRealSec — сколько реальных секунд осталось идти), а не
    // константная скорость сцены; paused — подход заморожен, актёр замирает
    // на месте (движение подхода — смысловое, на паузе живёт только косметика).
    void approachRetarget(Actor& a, double targetX, double remainRealSec, bool paused,
                          double now) const;
    // Отправить актёра к правому краю по дорожке выхода с настроением mood.
    void beginLeave(Actor& a, ActorState mood, double now) const;
    void rebuildView(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue,
                     double now);

    int width_;
    int sceneRows_;
    int exitLaneY_;                    // дорожка выхода (нижние строки сцены)
    bool effects_;                     // ui.scene_effects
    double timeScale_;                 // cfg.simulation.time_scale (для подхода)
    bool teleportNext_ = true;         // первый tick всегда расставляет мгновенно
    bool ticked_ = false;

    // Реестр актёров по id клиента. std::map — детерминированный порядок
    // обхода (важно для воспроизводимости кадров в тестах).
    std::map<ClientId, Actor> actors_;
    SceneView view_;
};

}  // namespace atmsim::scene
