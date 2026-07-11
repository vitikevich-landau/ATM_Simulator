#include "atmsim/console/scene/ScenePresenter.hpp"

#include <algorithm>
#include <cmath>

#include "atmsim/console/scene/ActorAnim.hpp"

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

// Грейс-период судьбы: столько тиков исчезнувший актёр ждёт свою запись в
// ленте операций (запись могла ещё не попасть в хвост к моменту снимка).
constexpr int kFateGraceTicks = 2;

// Допустимое расхождение твина подхода с оставшимся временем подхода из
// снимка, сек. Снимки опрашиваются реже кадров (refresh_hz), поэтому мелкий
// дрейф — норма (пересинхронизация на каждый тик дёргала бы актёра); большой
// (пауза/резюме, задержка опроса) — повод перестроить твин из текущей точки.
constexpr double kApproachResyncSec = 0.35;

// Доля оставшегося терпения, ниже которой клиент «нервничает» (переступает
// с ноги на ногу и получает бейдж «!»). Порог тот же, что красит полосу
// терпения в таблице красным.
constexpr double kNervousFrac = 0.15;

// Плавный ход easeInOut: разгон в начале, торможение у цели.
double smoothstep(double u) { return u * u * (3.0 - 2.0 * u); }

// Судьба клиента по хвосту ленты операций: последняя запись о нём.
enum class Fate { Unknown, Success, Fail };
Fate lookupFate(const std::vector<OperationRecord>& opsTail, ClientId id) {
    for (auto it = opsTail.rbegin(); it != opsTail.rend(); ++it) {
        if (it->clientId == id) return it->success ? Fate::Success : Fate::Fail;
    }
    return Fate::Unknown;
}

}  // namespace

ScenePresenter::ScenePresenter(int canvasWidth, int sceneRows, bool effects, double timeScale)
    : width_(canvasWidth),
      sceneRows_(sceneRows),
      // Дорожка выхода — три нижние строки сцены (спрайт высотой 3): уходящие
      // не толкутся на линии очереди, а «проходят перед камерой» внизу.
      exitLaneY_(sceneRows - 3),
      effects_(effects),
      // Конфиг гарантирует time_scale > 0 (ConfigLoader), но презентер зовут и
      // тесты с голыми числами — страхуемся от деления на ноль.
      timeScale_(timeScale > 0.0 ? timeScale : 1.0) {}

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
    // Catch-up: скорость не ниже ЛИЧНОЙ базовой (у каждого свой темп —
    // walkSpeedFactor), но и не медленнее, чем «успеть за kCatchUpSec» —
    // очередь из многих актёров продвигается дружно, хоть и не строем.
    const double speed = std::max(kWalkSpeed * walkSpeedFactor(a.id), dist / kCatchUpSec);
    a.tween = {cur, targetX, now, dist / speed};
}

void ScenePresenter::approachRetarget(Actor& a, double targetX, double remainRealSec,
                                      bool paused, double now) const {
    const double cur = posAt(a.tween, now);
    if (paused) {
        // Пауза замораживает подход в движке — актёр замирает на месте (после
        // resume снимок даст свежий остаток пути, и твин перестроится ниже).
        a.tween = {cur, cur, now, 0.0};
        return;
    }
    const bool newTarget = std::abs(targetX - a.tween.toX) > 1e-9;
    const double tweenRemain = std::max(0.0, a.tween.start + a.tween.dur - now);
    if (!newTarget && std::abs(tweenRemain - remainRealSec) <= kApproachResyncSec) {
        return;  // твин идёт в ногу со снимком — не дёргаем
    }
    if (std::abs(targetX - cur) < kArriveEps) {
        a.tween = {targetX, targetX, now, 0.0};
        return;
    }
    // Дорабатываем путь из текущей точки ровно за срок движка: момент «дошёл»
    // и момент старта обслуживания совпадают с точностью до опроса снимков.
    a.tween = {cur, targetX, now, std::max(remainRealSec, 0.0)};
    if (a.state != ActorState::WalkIn) a.state = ActorState::Advance;
}

void ScenePresenter::beginLeave(Actor& a, ActorState mood, double now) const {
    a.state = mood;
    a.y = exitLaneY_;
    a.stateSince = now;
    const double cur = posAt(a.tween, now);
    const double exitX = static_cast<double>(width_) + 2.0;
    // Личный темп масштабирует и скорость, и потолок длительности (медленному
    // позволено идти чуть дольше — иначе на широкой сцене потолок съедал бы
    // всю разницу темпов, и уходящие снова шагали бы строем).
    const double f = walkSpeedFactor(a.id);
    const double dur =
        std::min((exitX - cur) / (kWalkSpeed * kLeaveSpeedFactor * f), kLeaveMaxSec / f);
    a.tween = {cur, exitX, now, dur};
}

void ScenePresenter::tick(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue,
                          double nowSec, const std::vector<OperationRecord>& opsTail,
                          double snapshotAgeSec) {
    // Свежесть кэша снимков: возраст перестал расти — LiveRenderer переопросил
    // движок. Пока кэш устаревает, его содержимое ИЗМЕНИТЬСЯ не может — на
    // этом стоят и экстраполяция подхода, и расход грейса LeavePending ниже.
    const bool freshSnapshot = lastSnapshotAge_ < 0.0 || snapshotAgeSec <= lastSnapshotAge_;
    lastSnapshotAge_ = snapshotAgeSec;
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
    // ПОДХОД к банкомату (walk_seconds): движение подходящего — смысловое, его
    // темп задаёт снимок движка, а не константы сцены. remainReal — сколько
    // РЕАЛЬНЫХ секунд осталось идти. Остаток из КЭША снимков устарел ровно на
    // возраст кэша — вычитаем его (иначе при refresh_hz <= 2 дрейф твина
    // превышал бы kApproachResyncSec на каждом опросе, и подход дёргался бы
    // чередой пересинхронизаций). На паузе прогресс подхода заморожен и не
    // устаревает — возраст не вычитаем.
    const bool approaching = atm.approaching && atm.currentClientId.has_value();
    const double staleSec = (atm.state == AtmState::Paused) ? 0.0 : snapshotAgeSec;
    const double approachRemainReal =
        approaching
            ? std::max(0.0, (1.0 - atm.approachProgress) * atm.approachPlannedModelSec /
                                    timeScale_ -
                                staleSec)
            : 0.0;
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
        if (a.state == ActorState::LeaveHappy || a.state == ActorState::LeaveAngry ||
            a.state == ActorState::LeavePuzzled) {
            if (nowSec >= a.tween.start + a.tween.dur) {
                it = actors_.erase(it);
            } else {
                ++it;
            }
            continue;
        }

        // Ожидающие судьбу: запись об операции могла ещё не попасть в ленту к
        // моменту снимка — даём ей kFateGraceTicks СВЕЖИХ снимков. Успех ->
        // доволен, отказ -> растерян, записи так и нет -> обслуженный всё
        // равно уходит довольным (§4.5: операцию не бросают), ожидавший — не
        // дождался.
        if (a.state == ActorState::LeavePending) {
            if (targets.count(a.id) != 0) {
                // Ложная пропажа: snapshot() и queueSnapshot() читаются под
                // разными захватами мьютекса, и в микрозазор взятия из очереди
                // клиент мог не попасть НИ туда, НИ туда. Вернулся в снимок —
                // отменяем ожидание ухода и обслуживаем обычной веткой ниже.
                a.state = ActorState::QueueIdle;
                a.stateSince = nowSec;
            } else {
                const Fate fate = lookupFate(opsTail, a.id);
                if (fate == Fate::Success) {
                    beginLeave(a, ActorState::LeaveHappy, nowSec);
                } else if (fate == Fate::Fail) {
                    beginLeave(a, ActorState::LeavePuzzled, nowSec);
                } else if (freshSnapshot && --a.graceTicks <= 0) {
                    // Грейс тратится только на СВЕЖИХ снимках: между опросами
                    // кэш «вернуть» клиента не может, а кадры идут чаще
                    // опросов — иначе двухтиковый грейс истекал бы раньше
                    // первого же переопроса движка.
                    beginLeave(a, a.serving ? ActorState::LeaveHappy : ActorState::LeaveAngry,
                               nowSec);
                }
                ++it;
                continue;
            }
        }

        const auto found = targets.find(a.id);
        if (found == targets.end()) {
            // Исчез из снимка: замираем на месте и ждём судьбу из ленты.
            if (teleportNext_) {
                // В телепорт-режиме (старт/после overlay) уходы не проигрываем.
                it = actors_.erase(it);
                continue;
            }
            const double cur = posAt(a.tween, nowSec);
            a.state = ActorState::LeavePending;
            a.graceTicks = kFateGraceTicks;
            a.stateSince = nowSec;
            a.tween = {cur, cur, nowSec, 0.0};
            // Судьба может быть уже известна — не ждём тик впустую.
            const Fate fate = lookupFate(opsTail, a.id);
            if (fate == Fate::Success) beginLeave(a, ActorState::LeaveHappy, nowSec);
            else if (fate == Fate::Fail) beginLeave(a, ActorState::LeavePuzzled, nowSec);
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
        const bool actorApproaching = a.serving && approaching;
        if (teleportNext_) {
            if (actorApproaching) {
                // Телепорт-режим, но подход — смысловое состояние: ставим
                // актёра на его ИСТИННУЮ точку пути (по прогрессу из снимка)
                // и даём дойти в срок движка. «Стоит у банкомата, но таблица
                // пишет „подходит“» выглядело бы враньём.
                const double spawnX = static_cast<double>(width_) - 2.0;
                const double xNow = targetX + (spawnX - targetX) * (1.0 - atm.approachProgress);
                a.tween = {xNow, targetX, nowSec, std::max(approachRemainReal, 0.0)};
                a.state = ActorState::Advance;
            } else {
                a.tween = {targetX, targetX, nowSec, 0.0};
            }
        } else if (actorApproaching) {
            approachRetarget(a, targetX, approachRemainReal,
                             atm.state == AtmState::Paused, nowSec);
        } else if (std::abs(targetX - a.tween.toX) > 1e-9) {
            retarget(a, targetX, nowSec);
            if (a.state != ActorState::WalkIn) a.state = ActorState::Advance;
        }
        // Прибытие фиксируем стадией «на месте». Подходящий не становится
        // AtAtm, даже если его твин уже добежал (дрейф опроса снимков):
        // обслуживание ещё не началось — он стоит у терминала и ждёт.
        if (nowSec >= a.tween.start + a.tween.dur && !actorApproaching) {
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
        const bool spawnApproaching = t.serving && approaching;
        if (teleportNext_) {
            if (spawnApproaching) {
                // См. телепорт-ветку обхода реестра: подходящего ставим на его
                // истинную точку пути и даём дойти в срок движка.
                const double spawnX = static_cast<double>(width_) - 2.0;
                const double xNow = t.x + (spawnX - t.x) * (1.0 - atm.approachProgress);
                a.tween = {xNow, t.x, nowSec, std::max(approachRemainReal, 0.0)};
                a.state = ActorState::WalkIn;
            } else {
                a.tween = {t.x, t.x, nowSec, 0.0};
                a.state = t.serving ? ActorState::AtAtm : ActorState::QueueIdle;
            }
        } else {
            const double spawnX = static_cast<double>(width_) - 2.0;
            const double dist = std::abs(spawnX - t.x);
            // Подходящий (очередь была пуста, клиент идёт с «улицы» прямо к
            // терминалу) идёт в срок движка — без потолка kWalkInMaxSec:
            // синхронизация с движком важнее темпа входа. Обычный walk-in —
            // личным темпом; потолок масштабируется обратно темпу (см.
            // beginLeave), иначе на широкой сцене он съедал бы всю разницу.
            const double f = walkSpeedFactor(id);
            const double dur = spawnApproaching
                                   ? std::max(approachRemainReal, 0.0)
                                   : std::min(dist / (kWalkSpeed * f), kWalkInMaxSec / f);
            a.tween = {spawnX, t.x, nowSec, dur};
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
            if (s != ActorState::LeaveHappy && s != ActorState::LeaveAngry &&
                s != ActorState::LeavePuzzled && s != ActorState::LeavePending) {
                continue;
            }
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
    view_.effectsEnabled = effects_;
    // Фаза эффектов 0..7 от рендер-часов (~8 шагов/сек): спиннер, купюры, пар.
    view_.animPhase = static_cast<int>(std::fmod(now * 8.0, 8.0));

    const int slots = layout::visibleSlots(width_);
    const int shown = std::min<int>(slots, static_cast<int>(queue.size()));
    view_.hiddenQueueCount = queue.size() - static_cast<std::size_t>(shown);
    view_.overflowLabelX = layout::slotX(shown);

    // Кому пора нервничать: доля оставшегося терпения ниже порога (тот же
    // критерий, что красит полосу терпения в таблице красным).
    std::map<ClientId, bool> nervous;
    for (const ClientSnapshot& c : queue) {
        const double total = c.waitedSeconds + c.remainingPatience;
        nervous[c.id] = total > 0.0 && (c.remainingPatience / total) < kNervousFrac;
    }

    // Болтовня соседей по очереди: пары собираются по снимку очереди слева
    // направо; болтают только оба СТОЯЩИЕ на своих слотах (QueueIdle, твины
    // доиграны) и не нервничающие (им не до разговоров). Актёр участвует не
    // больше чем в одной паре: сосед, уже занятый разговором слева, левым
    // концом новой пары не становится. Расписание эпизодов — чистая функция
    // времени и пары (ActorAnim::chatState), состояния нет: продвижение
    // очереди переводит актёров в Advance и разрывает пару само.
    struct ChatInfo {
        bool speaking = false;
        int lean = 0;  // сдвиг спрайта к собеседнику, клеток (визуальное сближение)
    };
    std::map<ClientId, ChatInfo> chat;
    for (std::size_t i = 0; i + 1 < queue.size(); ++i) {
        const ClientId leftId = queue[i].id;
        const ClientId rightId = queue[i + 1].id;
        if (chat.count(leftId) != 0) continue;  // левый уже болтает с предыдущим
        const auto leftIt = actors_.find(leftId);
        const auto rightIt = actors_.find(rightId);
        if (leftIt == actors_.end() || rightIt == actors_.end()) continue;
        const auto standing = [now](const Actor& a) {
            return a.state == ActorState::QueueIdle && now >= a.tween.start + a.tween.dur;
        };
        if (!standing(leftIt->second) || !standing(rightIt->second)) continue;
        if (nervous[leftId] || nervous[rightId]) continue;
        const ChatRole role = chatState(leftId, rightId, now);
        if (role == ChatRole::None) continue;
        // Сближение: левый на клетку вправо, правый на две влево — между
        // спрайтами (3 колонки, шаг слотов 7) остаётся зазор в одну колонку.
        chat[leftId] = {role == ChatRole::LeftSpeaks, +1};
        chat[rightId] = {role == ChatRole::RightSpeaks, -2};
    }

    view_.actors.reserve(actors_.size());  // без цепочки реаллокаций push_back
    for (const auto& [id, a] : actors_) {
        SceneActorView av;
        const double x = posAt(a.tween, now);
        av.x = static_cast<int>(std::lround(x));
        av.y = a.y;
        av.label = "#" + std::to_string(id);
        av.tint = Tint::Default;
        av.labelTint = Tint::Grey;
        const bool moving = now < a.tween.start + a.tween.dur;
        // Кадр ног в движении чередуется по ПОЗИЦИИ, а не по времени: скорость
        // перебирания ног всегда совпадает со скоростью перемещения.
        const ActorPose walkFrame = (static_cast<long long>(std::floor(x)) % 2 == 0)
                                        ? ActorPose::WalkA
                                        : ActorPose::WalkB;
        switch (a.state) {
            case ActorState::AtAtm:
                // Акт по этапу обслуживания: «ручные» этапы — рука ходит к
                // панели, «машинные» — клиент ждёт и переминается.
                av.pose = moving ? walkFrame : pickActPose(id, view_.stage, now);
                av.tint = Tint::Cyan;
                av.bold = true;
                av.labelTint = Tint::Cyan;
                break;
            case ActorState::LeavePending:
                // Замер на месте в ожидании судьбы (доли секунды — незаметно).
                av.pose = ActorPose::Stand;
                break;
            case ActorState::LeaveHappy:
                // Уходит вприпрыжку: каждый третий «шаг» — руки вверх.
                av.pose = ((static_cast<long long>(std::floor(x)) / 3) % 3 == 0)
                              ? ActorPose::Cheer
                              : walkFrame;
                av.tint = Tint::Green;
                av.labelTint = Tint::Green;
                break;
            case ActorState::LeavePuzzled:
                av.pose = moving ? walkFrame : ActorPose::Stand;
                av.tint = Tint::Yellow;
                av.labelTint = Tint::Yellow;
                av.label += "?";  // «что это было?»
                break;
            case ActorState::LeaveAngry:
                av.pose = moving ? walkFrame : ActorPose::Stand;
                av.tint = Tint::Red;
                av.labelTint = Tint::Red;
                break;
            case ActorState::WalkIn:
            case ActorState::Advance:
            case ActorState::QueueIdle: {
                // По одному поиску на ключ (find вместо count + operator[]): и
                // быстрее, и без риска тихой вставки в map через operator[].
                const auto nervIt = nervous.find(id);
                const bool isNervous = nervIt != nervous.end() && nervIt->second;
                const auto chatIt = chat.find(id);
                if (moving) {
                    av.pose = walkFrame;
                } else if (isNervous) {
                    av.pose = pickNervousPose(id, now);
                } else if (chatIt != chat.end()) {
                    // Болтовня: сближение к собеседнику, говорящий жестикулирует
                    // и получает пузырёк речи над головой.
                    const ChatInfo& ci = chatIt->second;
                    av.x += ci.lean;
                    av.pose = pickChatPose(id, ci.speaking, now);
                    if (ci.speaking) av.chatBubble = chatBubble(now);
                } else {
                    // Живая очередь: переминания и редкие взмахи, у каждого
                    // своя фаза (хэш от id, без RNG).
                    av.pose = pickIdlePose(id, now);
                }
                av.nervous = isNervous;
                break;
            }
        }
        view_.actors.push_back(std::move(av));
    }
}

}  // namespace atmsim::scene
