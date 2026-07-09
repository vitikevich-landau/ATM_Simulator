#include "atmsim/console/scene/ScenePresenter.hpp"

#include <algorithm>
#include <cmath>

namespace atmsim::scene {
namespace {

// Базовая скорость ходьбы, клеток/сек. Быстрее — суетливо, медленнее — актёры
// не успевают за снимками при бойкой очереди.
constexpr double kWalkSpeed = 10.0;
// Максимум длительности «догоняющего» перехода: если слот уехал далеко, актёр
// не плетётся к нему базовой скоростью, а успевает за kCatchUpSec.
constexpr double kCatchUpSec = 0.7;
// Вход с края сцены не дольше этого (иначе на широком терминале walk-in полз бы
// несколько секунд и очередь визуально «запаздывала» бы за таблицей).
constexpr double kWalkInMaxSec = 1.2;
// Уходящие идут с этим множителем к базовой скорости и не дольше 3 секунд.
constexpr double kLeaveSpeedFactor = 1.6;
constexpr double kLeaveMaxSec = 3.0;
// Одновременно уходящих на сцене не больше этого числа — иначе при массовом
// уходе (ТО с renege, конец прогона) сцену запрудила бы толпа.
constexpr int kMaxLeavers = 4;
// Порог «уже на месте», клеток.
constexpr double kArriveEps = 0.5;

// Плавный ход easeInOut: разгон в начале, торможение у цели.
double smoothstep(double u) { return u * u * (3.0 - 2.0 * u); }

}  // namespace

ScenePresenter::ScenePresenter(int canvasWidth, int sceneRows)
    : width_(canvasWidth),
      sceneRows_(sceneRows),
      // Дорожка выхода — три нижние строки сцены (спрайт высотой 3): уходящие
      // не толкутся на линии очереди, а «проходят перед камерой» внизу.
      exitLaneY_(sceneRows - 3) {}

double ScenePresenter::posAt(const Tween& t, double now) {
    if (t.dur <= 0.0) return t.toX;
    const double u = (now - t.start) / t.dur;
    if (u <= 0.0) return t.fromX;
    if (u >= 1.0) return t.toX;
    return t.fromX + (t.toX - t.fromX) * smoothstep(u);
}

void ScenePresenter::retarget(Actor& a, double targetX, double now) const {
    // Новый твин стартует из ТЕКУЩЕЙ интерполированной точки — смена цели на
    // полпути не телепортирует и не дёргает актёра назад.
    const double cur = posAt(a.tween, now);
    const double dist = std::abs(targetX - cur);
    if (dist < kArriveEps) {
        a.tween = {targetX, targetX, now, 0.0};
        return;
    }
    // Дистанция больше полусцены — не бежим через весь экран, а телепортируемся:
    // снимок авторитетен, наглядность важнее красоты перехода.
    if (dist > width_ / 2.0) {
        a.tween = {targetX, targetX, now, 0.0};
        return;
    }
    // Catch-up: скорость не ниже базовой, но и не медленнее, чем «успеть за
    // kCatchUpSec» — очередь из многих актёров продвигается синхронно.
    const double speed = std::max(kWalkSpeed, dist / kCatchUpSec);
    a.tween = {cur, targetX, now, dist / speed};
}

void ScenePresenter::tick(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue,
                          double nowSec) {
    // --- 1. Целевые позиции по снимку (снимок авторитетен) -------------------
    struct Target {
        double x = 0.0;
        bool serving = false;
        bool hidden = false;  // хвост за пределами видимых слотов (виртуализация)
    };
    std::map<ClientId, Target> targets;
    if (atm.currentClientId) {
        targets[*atm.currentClientId] = {static_cast<double>(layout::kServeX), true, false};
    }
    const int slots = layout::visibleSlots(width_);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        // Обслуживаемый не может одновременно стоять в очереди, но на всякий
        // случай не перетираем его цель (снимки читаются в разные моменты).
        const ClientId id = queue[i].id;
        if (targets.count(id)) continue;
        targets[id] = {static_cast<double>(layout::slotX(static_cast<int>(i))),
                       false, static_cast<int>(i) >= slots};
    }

    // --- 2. Обход реестра: уход / ретаргет / виртуализация -------------------
    for (auto it = actors_.begin(); it != actors_.end();) {
        Actor& a = it->second;

        // Уже уходящие: доигрываем твин; дошёл до края — прощаемся.
        if (a.state == ActorState::LeaveHappy || a.state == ActorState::LeaveAngry) {
            if (nowSec >= a.tween.start + a.tween.dur) {
                it = actors_.erase(it);
            } else {
                ++it;
            }
            continue;
        }

        const auto found = targets.find(a.id);
        if (found == targets.end()) {
            // Исчез из снимка. Кто был на обслуживании — обслужен и доволен
            // (§4.5: начатую операцию не бросают); кто стоял в очереди — не
            // дождался. (Этап 4 уточнит судьбу по ленте операций.)
            if (teleportNext_) {
                // В телепорт-режиме (старт/после overlay) уходы не проигрываем.
                it = actors_.erase(it);
                continue;
            }
            a.state = a.serving ? ActorState::LeaveHappy : ActorState::LeaveAngry;
            a.y = exitLaneY_;
            a.stateSince = nowSec;
            const double cur = posAt(a.tween, nowSec);
            const double exitX = static_cast<double>(width_) + 2.0;
            const double dur =
                std::min((exitX - cur) / (kWalkSpeed * kLeaveSpeedFactor), kLeaveMaxSec);
            a.tween = {cur, exitX, nowSec, dur};
            ++it;
            continue;
        }

        if (found->second.hidden) {
            // Уехал за видимые слоты — не рисуем (его считает «… ещё N»).
            it = actors_.erase(it);
            continue;
        }

        a.serving = found->second.serving;
        const double targetX = found->second.x;
        if (teleportNext_) {
            a.tween = {targetX, targetX, nowSec, 0.0};
        } else if (std::abs(targetX - a.tween.toX) > 1e-9) {
            retarget(a, targetX, nowSec);
            if (a.state != ActorState::WalkIn) a.state = ActorState::Advance;
        }
        // Прибытие фиксируем стадией «на месте».
        if (nowSec >= a.tween.start + a.tween.dur) {
            a.state = a.serving ? ActorState::AtAtm : ActorState::QueueIdle;
        }
        ++it;
    }

    // --- 3. Спавн новых: вход с правого края ---------------------------------
    for (const auto& [id, t] : targets) {
        if (t.hidden || actors_.count(id) != 0) continue;
        Actor a;
        a.id = id;
        a.serving = t.serving;
        a.stateSince = nowSec;
        if (teleportNext_) {
            a.tween = {t.x, t.x, nowSec, 0.0};
            a.state = t.serving ? ActorState::AtAtm : ActorState::QueueIdle;
        } else {
            const double spawnX = static_cast<double>(width_) - 2.0;
            const double dist = std::abs(spawnX - t.x);
            a.tween = {spawnX, t.x, nowSec, std::min(dist / kWalkSpeed, kWalkInMaxSec)};
            a.state = ActorState::WalkIn;
        }
        actors_.emplace(id, a);
    }

    // --- 4. Не больше kMaxLeavers уходящих: старейший вытесняется мгновенно ---
    for (;;) {
        int leavers = 0;
        auto oldest = actors_.end();
        for (auto it = actors_.begin(); it != actors_.end(); ++it) {
            const ActorState s = it->second.state;
            if (s != ActorState::LeaveHappy && s != ActorState::LeaveAngry) continue;
            ++leavers;
            if (oldest == actors_.end() || it->second.stateSince < oldest->second.stateSince) {
                oldest = it;
            }
        }
        if (leavers <= kMaxLeavers) break;
        actors_.erase(oldest);
    }

    rebuildView(atm, queue, nowSec);
    teleportNext_ = false;
    ticked_ = true;
}

void ScenePresenter::rebuildView(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue,
                                 double now) {
    view_ = SceneView{};
    fillAtmState(view_, atm);

    const int slots = layout::visibleSlots(width_);
    const int shown = std::min<int>(slots, static_cast<int>(queue.size()));
    view_.hiddenQueueCount = queue.size() - static_cast<std::size_t>(shown);
    view_.overflowLabelX = layout::slotX(shown);

    for (const auto& [id, a] : actors_) {
        SceneActorView av;
        const double x = posAt(a.tween, now);
        av.x = static_cast<int>(std::lround(x));
        av.y = a.y;
        const bool moving = now < a.tween.start + a.tween.dur;
        // Поза: у банкомата — рука к нему; в движении — шаг. Кадр ног
        // чередуется по ПОЗИЦИИ, а не по времени: скорость перебирания ног
        // всегда совпадает со скоростью перемещения (включая catch-up).
        if (a.state == ActorState::AtAtm) {
            av.pose = ActorPose::ReachLeft;
        } else if (moving) {
            av.pose = (static_cast<long long>(std::floor(x)) % 2 == 0) ? ActorPose::WalkA
                                                                       : ActorPose::WalkB;
        } else {
            av.pose = ActorPose::Stand;
        }
        switch (a.state) {
            case ActorState::AtAtm:
                av.tint = Tint::Cyan;
                av.bold = true;
                av.labelTint = Tint::Cyan;
                break;
            case ActorState::LeaveHappy:
                av.tint = Tint::Green;
                av.labelTint = Tint::Green;
                break;
            case ActorState::LeaveAngry:
                av.tint = Tint::Red;
                av.labelTint = Tint::Red;
                break;
            default:
                av.tint = Tint::Default;
                av.labelTint = Tint::Grey;
                break;
        }
        av.label = "#" + std::to_string(id);
        view_.actors.push_back(std::move(av));
    }
}

}  // namespace atmsim::scene
