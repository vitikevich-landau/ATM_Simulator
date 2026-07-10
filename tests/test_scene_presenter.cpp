// ============================================================================
//  test_scene_presenter.cpp — тесты презентационного слоя актёров (этап 3).
//
//  Время подаётся в tick() параметром, поэтому все сценарии детерминированы:
//  «прошло 0.3 секунды» — это просто следующий вызов с nowSec+0.3. Движок не
//  нужен вовсе — снимки собираются руками, как их отдал бы AtmEngine.
// ============================================================================
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "atmsim/console/scene/ActorAnim.hpp"
#include "atmsim/console/scene/ScenePresenter.hpp"
#include "simple_test.hpp"

using namespace atmsim;
using scene::ScenePresenter;
using scene::SceneView;
namespace layout = atmsim::scene::layout;

namespace {

constexpr int kW = 100;   // ширина сцены в тестах
constexpr int kRows = 10;

AtmSnapshot serving(ClientId id) {
    AtmSnapshot s;
    s.state = AtmState::Serving;
    s.currentClientId = id;
    s.currentOperation = OperationType::CheckBalance;
    return s;
}

std::vector<ClientSnapshot> queueOf(std::initializer_list<int> ids) {
    std::vector<ClientSnapshot> q;
    for (int id : ids) {
        ClientSnapshot c;
        c.id = static_cast<ClientId>(id);
        q.push_back(c);
    }
    return q;
}

// Актёр с данной подписью (напр. "#3") или nullptr.
const scene::SceneActorView* findActor(const SceneView& v, const std::string& label) {
    for (const auto& a : v.actors) {
        if (a.label == label) return &a;
    }
    return nullptr;
}

// Стоит ли актёр «на месте» (этап 4 добавил живой idle: переминания и взмахи,
// поэтому у стоящего может быть любая из idle-поз, но не шаг и не акт).
bool isIdlePose(scene::ActorPose p) {
    return p == scene::ActorPose::Stand || p == scene::ActorPose::IdleShiftL ||
           p == scene::ActorPose::IdleShiftR || p == scene::ActorPose::Wave;
}

}  // namespace

// Первый tick — телепорт-режим: актёры сразу по местам, без анимаций входа.
TEST(presenter_first_tick_places_instantly) {
    ScenePresenter p(kW, kRows);
    CHECK(!p.hasTicked());
    p.tick(serving(1), queueOf({2, 3}), 0.0);
    CHECK(p.hasTicked());

    const scene::SceneActorView* a1 = findActor(p.view(), "#1");
    const scene::SceneActorView* a2 = findActor(p.view(), "#2");
    const scene::SceneActorView* a3 = findActor(p.view(), "#3");
    CHECK(a1 != nullptr);
    CHECK(a2 != nullptr);
    CHECK(a3 != nullptr);
    CHECK_EQ(a1->x, layout::kServeX);
    CHECK_EQ(a2->x, layout::slotX(0));
    CHECK_EQ(a3->x, layout::slotX(1));
    // Обслуживаемый — у банкомата (этап без снимка -> рука к панели),
    // остальные стоят в какой-то из idle-поз.
    CHECK(a1->pose == scene::ActorPose::ReachLeft);
    CHECK(isIdlePose(a2->pose));
}

// Новый клиент входит с правого края и ПЛАВНО доходит до слота.
TEST(presenter_walkin_moves_towards_slot) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2}), 0.0);   // телепорт-кадр

    p.tick(serving(1), queueOf({2, 3}), 1.0);  // пришёл #3
    // ВАЖНО: указатель findActor живёт внутри view_ и умирает на следующем
    // tick (view пересобирается) — значения копируем сразу.
    const scene::SceneActorView* spawn = findActor(p.view(), "#3");
    CHECK(spawn != nullptr);
    const int spawnX = spawn->x;
    CHECK(spawnX > layout::slotX(1) + 10);     // ещё далеко справа

    p.tick(serving(1), queueOf({2, 3}), 1.4);  // идёт
    const int midX = findActor(p.view(), "#3")->x;
    CHECK(midX < spawnX);                      // движется влево
    CHECK(midX > layout::slotX(1));

    p.tick(serving(1), queueOf({2, 3}), 5.0);  // давно дошёл
    CHECK_EQ(findActor(p.view(), "#3")->x, layout::slotX(1));
    CHECK(isIdlePose(findActor(p.view(), "#3")->pose));
}

// Очередь продвинулась — актёры плавно перетекают к новым слотам, без скачков.
TEST(presenter_advance_is_smooth) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2, 3, 4}), 0.0);

    // #1 обслужен и исчез, #2 пошёл на обслуживание, очередь сдвинулась.
    p.tick(serving(2), queueOf({3, 4}), 1.0);
    p.tick(serving(2), queueOf({3, 4}), 1.2);
    const scene::SceneActorView* a3 = findActor(p.view(), "#3");
    // #3 на полпути между старым slotX(1) и новым slotX(0).
    CHECK(a3->x < layout::slotX(1));
    CHECK(a3->x > layout::slotX(0));

    // Точные слоты проверяем в момент, когда пара (3,4) НЕ болтает: болтовня
    // (задача 3 сцены v2) визуально сближает стоящих соседей на клетку-две.
    double settled = 3.0;
    while (scene::chatState(3, 4, settled) != scene::ChatRole::None) settled += 0.5;
    p.tick(serving(2), queueOf({3, 4}), settled);
    CHECK_EQ(findActor(p.view(), "#3")->x, layout::slotX(0));
    CHECK_EQ(findActor(p.view(), "#4")->x, layout::slotX(1));
    // #2 дошёл до банкомата.
    CHECK_EQ(findActor(p.view(), "#2")->x, layout::kServeX);
    CHECK(findActor(p.view(), "#2")->pose == scene::ActorPose::ReachLeft);
}

// Обслуженный уходит по нижней дорожке зелёным и исчезает за краем; ожидавший,
// пропавший из очереди, уходит красным (после грейс-периода судьбы — записи в
// ленте у него нет и не будет).
TEST(presenter_leavers_walk_out_with_mood) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2, 3}), 0.0);

    // #1 обслужен (исчез; запись об успехе уже в ленте), #3 исчез из очереди.
    OperationRecord ok;
    ok.clientId = 1;
    ok.type = OperationType::CheckBalance;
    ok.success = true;
    const std::vector<OperationRecord> ops = {ok};

    p.tick(serving(2), queueOf({}), 1.0, ops);
    const scene::SceneActorView* happy = findActor(p.view(), "#1");
    CHECK(happy != nullptr);
    CHECK(happy->tint == scene::Tint::Green);  // судьба известна сразу
    CHECK_EQ(happy->y, kRows - 3);             // нижняя дорожка выхода
    const int happyX = happy->x;               // указатели умирают на следующем tick

    // #3 два тика ждёт свою запись (грейс), потом уходит красным.
    p.tick(serving(2), queueOf({}), 1.1, ops);
    p.tick(serving(2), queueOf({}), 1.2, ops);
    const scene::SceneActorView* angry = findActor(p.view(), "#3");
    CHECK(angry != nullptr);
    CHECK(angry->tint == scene::Tint::Red);
    CHECK_EQ(angry->y, kRows - 3);

    p.tick(serving(2), queueOf({}), 1.6, ops);
    CHECK(findActor(p.view(), "#1")->x > happyX);  // идут вправо

    p.tick(serving(2), queueOf({}), 10.0, ops);    // давно ушли
    CHECK(findActor(p.view(), "#1") == nullptr);
    CHECK(findActor(p.view(), "#3") == nullptr);
}

// Одновременных уходящих (включая ждущих судьбу) не больше четырёх —
// старейшие вытесняются мгновенно.
TEST(presenter_caps_simultaneous_leavers) {
    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, queueOf({1, 2, 3, 4, 5, 6}), 0.0);

    p.tick(AtmSnapshot{}, queueOf({}), 1.0);  // все шестеро исчезли разом
    CHECK(p.view().actors.size() <= std::size_t{4});

    // После грейса оставшиеся уходят красными — и их по-прежнему не больше 4.
    p.tick(AtmSnapshot{}, queueOf({}), 1.1);
    p.tick(AtmSnapshot{}, queueOf({}), 1.2);
    int leavers = 0;
    for (const auto& a : p.view().actors) {
        if (a.tint == scene::Tint::Red || a.tint == scene::Tint::Green) ++leavers;
    }
    CHECK(leavers <= 4);
    CHECK(leavers > 0);  // уходы реально проигрываются, а не съедены капом
}

// Хвост очереди за видимыми слотами не спавнится (виртуализация) и учтён в
// счётчике; вошедший в видимую зону появляется обычным walk-in.
TEST(presenter_virtualizes_hidden_tail) {
    ScenePresenter p(kW, kRows);
    std::vector<ClientSnapshot> q;
    for (int i = 1; i <= 20; ++i) {
        ClientSnapshot c;
        c.id = static_cast<ClientId>(i);
        q.push_back(c);
    }
    p.tick(AtmSnapshot{}, q, 0.0);
    const int slots = layout::visibleSlots(kW);
    CHECK_EQ(static_cast<int>(p.view().actors.size()), slots);
    CHECK_EQ(p.view().hiddenQueueCount, static_cast<std::size_t>(20 - slots));
    CHECK(findActor(p.view(), "#20") == nullptr);
}

// «Появился и исчез между тиками» — не спавнится вовсе (после телепорт-кадра
// это правило действует и для обычных кадров: клиента в снимке уже нет).
TEST(presenter_skips_flash_clients) {
    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, queueOf({1}), 0.0);
    // #2 пришёл и ушёл между нашими кадрами: в снимках его не было вообще.
    p.tick(AtmSnapshot{}, queueOf({1}), 0.5);
    CHECK(findActor(p.view(), "#2") == nullptr);
    CHECK_EQ(p.view().actors.size(), std::size_t{1});
}

// requestTeleport(): после overlay актёры расставляются мгновенно, уходы за
// время паузы не проигрываются.
TEST(presenter_teleport_after_request) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2, 3}), 0.0);

    p.requestTeleport();
    // За «время overlay» #1 обслужился и исчез, #2 у банкомата, #3 в слоте 0.
    p.tick(serving(2), queueOf({3}), 60.0);
    CHECK(findActor(p.view(), "#1") == nullptr);          // без анимации ухода
    CHECK_EQ(findActor(p.view(), "#2")->x, layout::kServeX);   // сразу на месте
    CHECK_EQ(findActor(p.view(), "#3")->x, layout::slotX(0));
}

// Детерминизм: одинаковая последовательность снимков и времени даёт побайтно
// одинаковые кадры (никаких скрытых часов/RNG внутри презентера).
TEST(presenter_is_deterministic) {
    auto run = [] {
        ScenePresenter p(kW, kRows);
        p.tick(serving(1), queueOf({2, 3, 4}), 0.0);
        p.tick(serving(2), queueOf({3, 4}), 0.4);
        p.tick(serving(2), queueOf({3, 4, 5}), 0.9);
        p.tick(serving(3), queueOf({4, 5}), 1.7);
        std::string dump;
        for (const auto& a : p.view().actors) {
            dump += a.label + ":" + std::to_string(a.x) + ":" + std::to_string(a.y) + ":" +
                    std::to_string(static_cast<int>(a.pose)) + ":" +
                    std::to_string(static_cast<int>(a.tint)) + ";";
        }
        return dump;
    };
    CHECK_EQ(run(), run());
}

// ---------------------------------------------------------------------------
// Этап 4: судьбы по ленте операций, нервозность, живые позы.
// ---------------------------------------------------------------------------

namespace {

OperationRecord record(ClientId id, bool success) {
    OperationRecord r;
    r.clientId = id;
    r.type = OperationType::Withdraw;
    r.success = success;
    if (!success) r.errorMessage = "нет денег";
    return r;
}

}  // namespace

// Судьба по ленте: успех — зелёный, отказ — жёлтый с «?» в подписи,
// без записи — красный (не дождался) после грейс-периода.
TEST(presenter_fates_from_operations_tail) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2, 3}), 0.0);

    // #1 обслужен успешно, #3 исчез из очереди без записи.
    std::vector<OperationRecord> ops = {record(1, true)};
    p.tick(serving(2), queueOf({}), 0.1, ops);
    CHECK(findActor(p.view(), "#1")->tint == scene::Tint::Green);

    // #3 ждёт судьбу (грейс): ещё не уходит и не красный.
    const scene::SceneActorView* pending = findActor(p.view(), "#3");
    CHECK(pending != nullptr);
    CHECK(pending->tint == scene::Tint::Default);

    // Грейс истёк (2 тика без записи) — не дождался, уходит красным.
    p.tick(serving(2), queueOf({}), 0.2, ops);
    p.tick(serving(2), queueOf({}), 0.3, ops);
    CHECK(findActor(p.view(), "#3")->tint == scene::Tint::Red);
}

// Отказ операции: клиент уходит растерянным (жёлтый, «#id?»).
TEST(presenter_failed_operation_leaves_puzzled) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({}), 0.0);

    std::vector<OperationRecord> ops = {record(1, false)};
    p.tick(AtmSnapshot{}, queueOf({}), 0.1, ops);
    const scene::SceneActorView* puzzled = findActor(p.view(), "#1?");
    CHECK(puzzled != nullptr);
    CHECK(puzzled->tint == scene::Tint::Yellow);
}

// Терпение на исходе — бейдж нервозности и «дёрганая» поза.
TEST(presenter_marks_nervous_clients) {
    ScenePresenter p(kW, kRows);
    std::vector<ClientSnapshot> q = queueOf({1, 2});
    q[0].waitedSeconds = 10.0;
    q[0].remainingPatience = 100.0;   // спокоен (доля ~0.9)
    q[1].waitedSeconds = 95.0;
    q[1].remainingPatience = 5.0;     // на пределе (доля 0.05 < 0.15)
    p.tick(AtmSnapshot{}, q, 0.0);
    CHECK(!findActor(p.view(), "#1")->nervous);
    CHECK(findActor(p.view(), "#2")->nervous);
}

// Idle-анимации детерминированы и разнообразны: одна и та же пара (id, t)
// всегда даёт одну позу; на длинном окне встречаются и переминания, и взмах.
TEST(actor_anim_idle_is_deterministic_and_varied) {
    using scene::ActorPose;
    bool sawShift = false;
    bool sawWave = false;
    for (int i = 0; i < 400; ++i) {
        const double t = i * 0.25;
        const ActorPose a = scene::pickIdlePose(7, t);
        const ActorPose b = scene::pickIdlePose(7, t);
        CHECK(a == b);  // детерминизм
        if (a == ActorPose::IdleShiftL || a == ActorPose::IdleShiftR) sawShift = true;
        if (a == ActorPose::Wave) sawWave = true;
    }
    CHECK(sawShift);
    CHECK(sawWave);
    // Разные клиенты — разные фазы: в один момент позы не обязаны совпадать
    // (проверяем, что за окно хоть раз разошлись).
    bool diverged = false;
    for (int i = 0; i < 40 && !diverged; ++i) {
        diverged = scene::pickIdlePose(1, i * 0.5) != scene::pickIdlePose(2, i * 0.5);
    }
    CHECK(diverged);
}

// Акты у банкомата: «ручной» этап шевелит рукой (позы чередуются во времени),
// «машинный» — переминание, как в очереди.
TEST(actor_anim_act_poses_by_stage) {
    using scene::ActorPose;
    // EnterPin — ручной: на протяжении секунды должны встретиться обе позы.
    bool sawReach = false;
    bool sawStand = false;
    for (int i = 0; i < 8; ++i) {
        const ActorPose pose = scene::pickActPose(1, ServiceStage::EnterPin, i * 0.25);
        if (pose == ActorPose::ReachLeft) sawReach = true;
        if (pose == ActorPose::Stand) sawStand = true;
    }
    CHECK(sawReach);
    CHECK(sawStand);
    // BankRequest — машинный: рука к панели не тянется.
    for (int i = 0; i < 8; ++i) {
        CHECK(scene::pickActPose(1, ServiceStage::BankRequest, i * 0.25) !=
              ActorPose::ReachLeft);
    }
}

// --- Подход к банкомату (walk_seconds): темп движения задаёт движок ----------

namespace {

// Снимок «клиент взят из очереди и ИДЁТ к банкомату» (этапа ещё нет).
AtmSnapshot approachingTo(ClientId id, double progress, double plannedModelSec) {
    AtmSnapshot s = serving(id);
    s.approaching = true;
    s.approachProgress = progress;
    s.approachPlannedModelSec = plannedModelSec;
    return s;
}

}  // namespace

// Клиент из головы очереди идёт к банкомату РОВНО срок подхода из снимка (а не
// константной скоростью сцены) и не подсвечивается как «обслуживаемый»
// (AtAtm, голубой), пока подход не завершён — даже если твин уже добежал.
TEST(presenter_approach_follows_engine_schedule) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2}), 0.0);  // телепорт-кадр: #2 в слоте 0

    // #1 ушёл; #2 взят из очереди и подходит: 2 модельные секунды пути
    // (базовой скоростью сцены он дошёл бы от слота 0 за ~0.8 c).
    p.tick(approachingTo(2, 0.0, 2.0), {}, 1.0);
    p.tick(approachingTo(2, 0.5, 2.0), {}, 2.0);   // середина срока
    const scene::SceneActorView* mid = findActor(p.view(), "#2");
    CHECK(mid != nullptr);
    CHECK(mid->x > layout::kServeX);               // ещё в пути
    CHECK(mid->x < layout::slotX(0));
    CHECK(mid->tint != scene::Tint::Cyan);         // не «обслуживается»

    // Подход завершён по движку, но обслуживание не началось: актёр у
    // банкомата, но без подсветки обслуживания — стоит и ждёт.
    p.tick(approachingTo(2, 1.0, 2.0), {}, 3.1);
    const scene::SceneActorView* arrived = findActor(p.view(), "#2");
    CHECK_EQ(arrived->x, layout::kServeX);
    CHECK(arrived->tint != scene::Tint::Cyan);

    // Обслуживание началось (подход погашен, появился этап) — теперь AtAtm.
    AtmSnapshot srv = serving(2);
    srv.currentStage = ServiceStage::InsertCard;
    srv.serviceProgress = 0.05;
    p.tick(srv, {}, 3.3);
    const scene::SceneActorView* atAtm = findActor(p.view(), "#2");
    CHECK_EQ(atAtm->x, layout::kServeX);
    CHECK(atAtm->tint == scene::Tint::Cyan);
}

// Очередь была пуста: новый клиент входит с правого края и идёт к терминалу в
// срок движка — БЕЗ потолка kWalkInMaxSec (синхронизация с движком важнее
// темпа входа: обычный walk-in дошёл бы максимум за 1.2 c).
TEST(presenter_approach_spawn_walks_from_edge_on_schedule) {
    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, {}, 0.0);               // телепорт-кадр: сцена пуста

    p.tick(approachingTo(9, 0.0, 3.0), {}, 1.0);  // 3 c пути от края
    const scene::SceneActorView* spawned = findActor(p.view(), "#9");
    CHECK(spawned != nullptr);
    const int spawnX = spawned->x;
    CHECK(spawnX > layout::kServeX + 40);          // ещё далеко справа

    p.tick(approachingTo(9, 0.5, 3.0), {}, 2.5);   // середина срока
    const int midX = findActor(p.view(), "#9")->x;
    CHECK(midX < spawnX);                          // движется к банкомату
    CHECK(midX > layout::kServeX + 20);            // но потолок 1.2 c уже позади

    p.tick(approachingTo(9, 1.0, 3.0), {}, 4.1);   // дошёл точно в срок
    CHECK_EQ(findActor(p.view(), "#9")->x, layout::kServeX);
}

// Пауза замораживает подход (движение — смысловое, §4.6): актёр замирает на
// месте, после resume дорабатывает остаток пути в срок движка.
TEST(presenter_approach_freezes_on_pause) {
    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, {}, 0.0);               // телепорт-кадр: сцена пуста

    p.tick(approachingTo(5, 0.0, 4.0), {}, 1.0);  // идёт: 4 c пути

    AtmSnapshot paused = approachingTo(5, 0.25, 4.0);
    paused.state = AtmState::Paused;
    p.tick(paused, {}, 2.0);
    const int frozenX = findActor(p.view(), "#5")->x;
    p.tick(paused, {}, 2.7);
    CHECK_EQ(findActor(p.view(), "#5")->x, frozenX);  // стоит на месте

    // Resume: остаток пути проходит за остаток срока движка.
    p.tick(approachingTo(5, 0.25, 4.0), {}, 3.0);
    p.tick(approachingTo(5, 0.5, 4.0), {}, 5.0);
    const int resumedX = findActor(p.view(), "#5")->x;
    CHECK(resumedX < frozenX);                     // снова движется к банкомату
    CHECK(resumedX > layout::kServeX);
    p.tick(approachingTo(5, 1.0, 4.0), {}, 7.2);
    CHECK_EQ(findActor(p.view(), "#5")->x, layout::kServeX);
}

// Телепорт-режим (первый кадр live-сессии, resume) ставит ПОДХОДЯЩЕГО не к
// банкомату, а на его истинную точку пути по прогрессу из снимка: «уже стоит
// у терминала», когда таблица пишет «подходит», — враньё.
TEST(presenter_teleport_places_approaching_on_path) {
    ScenePresenter p(kW, kRows);
    p.tick(approachingTo(3, 0.5, 2.0), {}, 0.0);  // первый tick — телепорт
    const scene::SceneActorView* a = findActor(p.view(), "#3");
    CHECK(a != nullptr);
    CHECK(a->x > layout::kServeX + 10);            // не у банкомата
    CHECK(a->x < kW - 12);                         // и не у края — на полпути

    // Дошагивает остаток в срок движка и ждёт начала обслуживания.
    p.tick(approachingTo(3, 1.0, 2.0), {}, 1.1);
    CHECK_EQ(findActor(p.view(), "#3")->x, layout::kServeX);
    CHECK(findActor(p.view(), "#3")->tint != scene::Tint::Cyan);
}

// --- Личный темп ходьбы (walkSpeedFactor) ------------------------------------

// Множитель темпа детерминирован, лежит в заявленном диапазоне и реально
// различается между клиентами (не все шагают строем).
TEST(actor_anim_walk_speed_factor_is_personal) {
    double minF = 10.0, maxF = 0.0;
    for (int id = 1; id <= 50; ++id) {
        const double f = scene::walkSpeedFactor(static_cast<ClientId>(id));
        CHECK(f >= 0.75);
        CHECK(f <= 1.36);
        CHECK_EQ(f, scene::walkSpeedFactor(static_cast<ClientId>(id)));
        minF = std::min(minF, f);
        maxF = std::max(maxF, f);
    }
    CHECK(maxF - minF > 0.2);
}

// Два одновременно вошедших актёра доходят до слотов за РАЗНОЕ время: длина
// walk-in ограничена потолком, масштабируемым личным темпом (1.2 c / f), —
// быстрый уже стоит на месте, медленный ещё в пути.
TEST(presenter_walkin_speed_is_personal) {
    // Детерминированно подбираем пару id с заметно разными темпами.
    ClientId fast = 1, slow = 1;
    for (ClientId id = 1; id <= 40; ++id) {
        if (scene::walkSpeedFactor(id) > scene::walkSpeedFactor(fast)) fast = id;
        if (scene::walkSpeedFactor(id) < scene::walkSpeedFactor(slow)) slow = id;
    }
    CHECK(scene::walkSpeedFactor(fast) - scene::walkSpeedFactor(slow) > 0.2);

    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, {}, 0.0);  // телепорт-кадр: сцена пуста
    const std::vector<ClientSnapshot> q =
        queueOf({static_cast<int>(slow), static_cast<int>(fast)});
    const std::string slowLabel = "#" + std::to_string(slow);
    const std::string fastLabel = "#" + std::to_string(fast);

    // Оба входят одновременно с правого края (дистанции до слотов велики,
    // работает потолок — длительность зависит только от темпа).
    double fastArrivedAt = -1.0;
    for (double t = 1.0; t <= 4.0; t += 0.05) {
        p.tick(AtmSnapshot{}, q, t);
        const bool fastAtSlot = findActor(p.view(), fastLabel)->x == layout::slotX(1);
        const bool slowAtSlot = findActor(p.view(), slowLabel)->x == layout::slotX(0);
        if (fastAtSlot && fastArrivedAt < 0.0) {
            fastArrivedAt = t;
            CHECK(!slowAtSlot);  // медленный в этот момент ещё в пути
        }
    }
    CHECK(fastArrivedAt > 0.0);  // быстрый дошёл в пределах прогона
}

// --- Болтовня соседей по очереди ----------------------------------------------

// Расписание болтовни детерминировано; на длинном горизонте у пары есть и
// молчание, и эпизоды, где говорят ОБА (роли чередуются каждые ~1.7 c).
TEST(actor_anim_chat_schedule_is_deterministic_and_alternates) {
    bool sawNone = false, sawLeft = false, sawRight = false;
    for (double t = 0.0; t < 120.0; t += 0.25) {
        const scene::ChatRole r = scene::chatState(2, 3, t);
        CHECK(r == scene::chatState(2, 3, t));  // чистая функция времени
        if (r == scene::ChatRole::None) sawNone = true;
        if (r == scene::ChatRole::LeftSpeaks) sawLeft = true;
        if (r == scene::ChatRole::RightSpeaks) sawRight = true;
    }
    CHECK(sawNone);
    CHECK(sawLeft);
    CHECK(sawRight);
}

// Скучающие соседи болтают: сближаются на клетку-две, ровно у одного из
// собеседников (говорящего) пузырёк речи; вне эпизода стоят ровно по слотам.
TEST(presenter_neighbors_chat_when_idle) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2, 3}), 0.0);  // телепорт: все стоят по местам

    bool sawChat = false;
    for (double t = 0.5; t < 120.0; t += 0.5) {
        p.tick(serving(1), queueOf({2, 3}), t);
        const scene::SceneActorView* a2 = findActor(p.view(), "#2");
        const scene::SceneActorView* a3 = findActor(p.view(), "#3");
        const bool leaned =
            a2->x == layout::slotX(0) + 1 && a3->x == layout::slotX(1) - 2;
        if (!leaned) {
            // Вне эпизода — ровно по слотам и без пузырьков.
            CHECK_EQ(a2->x, layout::slotX(0));
            CHECK_EQ(a3->x, layout::slotX(1));
            CHECK(a2->chatBubble == 0);
            CHECK(a3->chatBubble == 0);
            continue;
        }
        if (!sawChat) {
            sawChat = true;
            const int bubbles = (a2->chatBubble != 0 ? 1 : 0) + (a3->chatBubble != 0 ? 1 : 0);
            CHECK_EQ(bubbles, 1);
        }
    }
    CHECK(sawChat);
}

// Нервные (терпение на исходе) в разговоры не вступают: ни сближения, ни
// пузырьков — только переминание и бейдж «!».
TEST(presenter_nervous_do_not_chat) {
    ScenePresenter p(kW, kRows);
    std::vector<ClientSnapshot> q = queueOf({2, 3});
    for (ClientSnapshot& c : q) {
        c.waitedSeconds = 95.0;      // осталось 5% терпения — ниже порога 15%
        c.remainingPatience = 5.0;
    }
    p.tick(serving(1), q, 0.0);
    for (double t = 0.5; t < 60.0; t += 0.5) {
        p.tick(serving(1), q, t);
        CHECK_EQ(findActor(p.view(), "#2")->x, layout::slotX(0));
        CHECK_EQ(findActor(p.view(), "#3")->x, layout::slotX(1));
        CHECK(findActor(p.view(), "#2")->chatBubble == 0);
        CHECK(findActor(p.view(), "#3")->chatBubble == 0);
    }
}

// --- Кэш снимков реже кадров (snapshotAgeSec) ---------------------------------

// Подход при редких опросах движка (refresh_hz=2, кадры чаще): остаток пути из
// устаревшего кэша экстраполируется возрастом снимка — твин НЕ дёргается
// пересинхронизациями, актёр идёт монотонно и приходит точно в срок движка.
TEST(presenter_approach_smooth_with_stale_snapshot_cache) {
    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, {}, 0.0);  // телепорт-кадр: сцена пуста

    // Подход 3 c; опрос движка каждые 0.5 c, кадры каждые 0.1 c.
    const double t0 = 1.0, planned = 3.0;
    int lastX = kW;  // движение справа налево: x строго не растёт
    for (double t = t0; t <= t0 + planned + 0.001; t += 0.1) {
        const double sincePoll = std::fmod(t - t0 + 1e-9, 0.5);
        const double pollT = t - sincePoll;
        const double progress = std::min((pollT - t0) / planned, 1.0);
        p.tick(approachingTo(7, progress, planned), {}, t, {}, sincePoll);
        const int x = findActor(p.view(), "#7")->x;
        CHECK(x <= lastX);  // без откатов и рывков назад
        lastX = x;
    }
    // К концу срока движка актёр ровно у банкомата (без финального «телепорта»).
    CHECK_EQ(lastX, layout::kServeX);
}

// Ложная пропажа клиента из-за перекоса чтения snapshot()/queueSnapshot() в
// момент взятия из очереди: пока кэш снимков не переопрошен (возраст растёт),
// грейс LeavePending не тратится, а на первом же свежем снимке клиент
// возвращается в строй — никакого «ушёл злым и вошёл заново».
TEST(presenter_leave_pending_survives_snapshot_read_skew) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2}), 0.0);  // телепорт: #1 у банкомата, #2 в слоте

    // Перекошенная пара: #2 нет НИ в текущих, НИ в очереди (движок взял его из
    // очереди между двумя чтениями). Свежий опрос в t=1.0, дальше кадры 15 fps
    // со стареющим кэшем.
    p.tick(AtmSnapshot{}, {}, 1.0, {}, 0.0);
    p.tick(AtmSnapshot{}, {}, 1.066, {}, 0.066);
    p.tick(AtmSnapshot{}, {}, 1.133, {}, 0.133);
    p.tick(AtmSnapshot{}, {}, 1.2, {}, 0.2);
    {
        // #2 всё ещё на сцене и не ушёл злым (грейс не истёк на устаревшем кэше).
        const scene::SceneActorView* a2 = findActor(p.view(), "#2");
        CHECK(a2 != nullptr);
        CHECK(a2->tint != scene::Tint::Red);
        CHECK_EQ(a2->y, layout::kActorTopY);  // не на дорожке выхода
    }

    // Следующий опрос движка (свежий снимок) приносит правду: #2 подходит к
    // банкомату — ожидание ухода отменяется, актёр продолжает жить.
    p.tick(approachingTo(2, 0.1, 2.0), {}, 1.25, {}, 0.0);
    p.tick(approachingTo(2, 0.5, 2.0), {}, 2.05, {}, 0.0);
    const scene::SceneActorView* a2 = findActor(p.view(), "#2");
    CHECK(a2 != nullptr);
    CHECK(a2->tint != scene::Tint::Red);
    CHECK_EQ(a2->y, layout::kActorTopY);
    CHECK(a2->x <= layout::slotX(0));  // идёт от слота к банкомату
}
