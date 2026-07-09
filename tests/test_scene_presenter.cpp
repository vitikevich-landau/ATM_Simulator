// ============================================================================
//  test_scene_presenter.cpp — тесты презентационного слоя актёров (этап 3).
//
//  Время подаётся в tick() параметром, поэтому все сценарии детерминированы:
//  «прошло 0.3 секунды» — это просто следующий вызов с nowSec+0.3. Движок не
//  нужен вовсе — снимки собираются руками, как их отдал бы AtmEngine.
// ============================================================================
#include <algorithm>
#include <string>
#include <vector>

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
    // Обслуживаемый — у банкомата с протянутой рукой, остальные стоят.
    CHECK(a1->pose == scene::ActorPose::ReachLeft);
    CHECK(a2->pose == scene::ActorPose::Stand);
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
    CHECK(findActor(p.view(), "#3")->pose == scene::ActorPose::Stand);
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

    p.tick(serving(2), queueOf({3, 4}), 3.0);
    CHECK_EQ(findActor(p.view(), "#3")->x, layout::slotX(0));
    CHECK_EQ(findActor(p.view(), "#4")->x, layout::slotX(1));
    // #2 дошёл до банкомата.
    CHECK_EQ(findActor(p.view(), "#2")->x, layout::kServeX);
    CHECK(findActor(p.view(), "#2")->pose == scene::ActorPose::ReachLeft);
}

// Обслуженный уходит по нижней дорожке зелёным и исчезает за краем; ожидавший,
// пропавший из очереди, уходит красным.
TEST(presenter_leavers_walk_out_with_mood) {
    ScenePresenter p(kW, kRows);
    p.tick(serving(1), queueOf({2, 3}), 0.0);

    // #1 обслужен (исчез), #3 не дождался (исчез из очереди).
    p.tick(serving(2), queueOf({}), 1.0);
    const scene::SceneActorView* happy = findActor(p.view(), "#1");
    const scene::SceneActorView* angry = findActor(p.view(), "#3");
    CHECK(happy != nullptr);
    CHECK(angry != nullptr);
    CHECK(happy->tint == scene::Tint::Green);
    CHECK(angry->tint == scene::Tint::Red);
    CHECK_EQ(happy->y, kRows - 3);   // нижняя дорожка выхода
    CHECK_EQ(angry->y, kRows - 3);
    const int happyX = happy->x;     // указатели умирают на следующем tick

    p.tick(serving(2), queueOf({}), 1.5);
    CHECK(findActor(p.view(), "#1")->x > happyX);  // идут вправо

    p.tick(serving(2), queueOf({}), 10.0);           // давно ушли
    CHECK(findActor(p.view(), "#1") == nullptr);
    CHECK(findActor(p.view(), "#3") == nullptr);
}

// Одновременных уходящих не больше четырёх — старейшие вытесняются мгновенно.
TEST(presenter_caps_simultaneous_leavers) {
    ScenePresenter p(kW, kRows);
    p.tick(AtmSnapshot{}, queueOf({1, 2, 3, 4, 5, 6}), 0.0);

    p.tick(AtmSnapshot{}, queueOf({}), 1.0);  // все шестеро исчезли разом
    int leavers = 0;
    for (const auto& a : p.view().actors) {
        if (a.tint == scene::Tint::Red || a.tint == scene::Tint::Green) ++leavers;
    }
    CHECK(leavers <= 4);
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
