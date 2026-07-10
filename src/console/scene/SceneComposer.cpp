#include "atmsim/console/scene/SceneComposer.hpp"

#include <algorithm>
#include <cmath>

#include "atmsim/console/scene/GlyphSet.hpp"

namespace atmsim::scene {
namespace {

// Ширина строки в КОЛОНКАХ терминала = числу кодовых точек (GlyphSet
// гарантирует одноколоночность каждого глифа).
int columnsOf(const std::string& utf8Text) {
    return static_cast<int>(utf8::decode(utf8Text).size());
}

// Пишет текст с центрированием в поле [x, x + fieldWidth).
void textCentered(SceneCanvas& canvas, int x, int y, int fieldWidth, const std::string& utf8Text,
                  Tint tint, bool bold = false) {
    const int pad = (fieldWidth - columnsOf(utf8Text)) / 2;
    canvas.text(x + (pad > 0 ? pad : 0), y, utf8Text, tint, bold);
}

// Строка прогресса для экрана банкомата: «▓▓▓░░░░ 52%» (12 колонок).
std::string screenProgressLine(double progress) {
    if (progress < 0.0) progress = 0.0;
    if (progress > 1.0) progress = 1.0;
    constexpr int kBarWidth = 7;
    const int filled = static_cast<int>(std::lround(progress * kBarWidth));
    std::string line;
    for (int i = 0; i < kBarWidth; ++i) line += (i < filled) ? "▓" : "░";
    const int pct = static_cast<int>(std::lround(progress * 100.0));
    std::string pctText = std::to_string(pct) + "%";
    while (pctText.size() < 4) pctText.insert(pctText.begin(), ' ');  // « 52%»
    return line + " " + pctText;
}

// Экран банкомата: три строки по kAtmScreenWidth колонок в зависимости от
// состояния. Пустая строка = пустая строка экрана.
struct ScreenContent {
    std::array<std::string, 3> lines;
    Tint tint = Tint::Cyan;
};

ScreenContent screenContent(const SceneView& v) {
    switch (v.state) {
        case AtmState::Serving:
            // Клиент ещё только ИДЁТ к банкомату (walk_seconds): для подходящего
            // человека экран пока «СВОБОДНО» — переключится, когда тот вставит
            // карту (начнётся первый этап обслуживания).
            if (v.approaching) break;
            if (v.stage) {
                const std::array<std::string, 2> t = screenStageText(*v.stage);
                return {{t[0], t[1], screenProgressLine(v.progress)}, Tint::Cyan};
            }
            return {{"", "ОБСЛУЖИВАНИЕ", ""}, Tint::Cyan};
        case AtmState::Paused:
            return {{"", "ПАУЗА", ""}, Tint::Yellow};
        case AtmState::Maintenance: {
            std::string eta;
            if (v.maintenanceEtaSeconds < 0.0) eta = "до команды";
            else eta = "~" + std::to_string(static_cast<long>(v.maintenanceEtaSeconds)) + "с";
            return {{"ТЕХРАБОТЫ", eta, ""}, Tint::Blue};
        }
        case AtmState::Stopped:
            return {{"", "ОСТАНОВЛЕН", ""}, Tint::Grey};
        case AtmState::Idle:
            break;
    }
    // Простой: приглашаем; при низкой кассе экран честно предупреждает.
    return {{"", "СВОБОДНО", v.lowCash ? "КАССА НИЗКА" : ""}, Tint::Green};
}

}  // namespace

void fillAtmState(SceneView& view, const AtmSnapshot& atm) {
    view.state = atm.state;
    view.stage = atm.currentStage;
    view.progress = atm.serviceProgress;
    view.approaching = atm.approaching;
    view.lowCash = atm.lowCash;
    view.maintenancePending = atm.maintenancePending;
    view.maintenanceEtaSeconds = atm.maintenanceEtaSeconds;
}

SceneView buildSceneView(const AtmSnapshot& atm, const std::vector<ClientSnapshot>& queue,
                         int canvasWidth) {
    SceneView v;
    fillAtmState(v, atm);

    // Обслуживаемый клиент — у корпуса, рука к банкомату.
    if (atm.currentClientId) {
        SceneActorView a;
        a.x = layout::kServeX;
        a.pose = ActorPose::ReachLeft;
        a.tint = Tint::Cyan;
        a.bold = true;
        a.label = "#" + std::to_string(*atm.currentClientId);
        a.labelTint = Tint::Cyan;
        v.actors.push_back(std::move(a));
    }

    // Очередь: первые visibleSlots человечков, остальные — в счётчик «… ещё N».
    const int slots = layout::visibleSlots(canvasWidth);
    const int shown = std::min<int>(slots, static_cast<int>(queue.size()));
    for (int i = 0; i < shown; ++i) {
        SceneActorView a;
        a.x = layout::slotX(i);
        a.pose = ActorPose::Stand;
        a.label = "#" + std::to_string(queue[static_cast<std::size_t>(i)].id);
        v.actors.push_back(std::move(a));
    }
    v.hiddenQueueCount = queue.size() - static_cast<std::size_t>(shown);
    v.overflowLabelX = layout::slotX(shown);
    return v;
}

void composeScene(const SceneView& view, SceneCanvas& canvas) {
    canvas.clear();

    // --- Корпус банкомата (непрозрачно, построчно через text) ----------------
    const std::vector<std::string>& art = atmArtRows();
    for (std::size_t row = 0; row < art.size(); ++row) {
        canvas.text(layout::kAtmX, static_cast<int>(row), art[row]);
    }
    // Шильдик. При запрошенном, но ещё не начатом ТО (§4.5: текущего клиента
    // дорабатываем) — предупреждение вместо названия: банкомат «уходит на ТО».
    if (view.maintenancePending) {
        textCentered(canvas, layout::kAtmX + 1, 1, kAtmArtWidth - 2, "→ ТЕХРАБОТЫ", Tint::Blue,
                     /*bold=*/true);
    } else {
        textCentered(canvas, layout::kAtmX + 1, 1, kAtmArtWidth - 2, "БАНКОМАТ", Tint::Default,
                     /*bold=*/true);
    }

    // --- Экран (3 строки по kAtmScreenWidth колонок, x с учётом двойной рамки) --
    const ScreenContent screen = screenContent(view);
    const int screenX = layout::kAtmX + 2;
    for (int row = 0; row < 3; ++row) {
        textCentered(canvas, screenX, 3 + row, kAtmScreenWidth,
                     screen.lines[static_cast<std::size_t>(row)], screen.tint);
    }

    // --- Эффект: спиннер связи с банком в углу экрана --------------------------
    // Живой индикатор «банкомат думает» на этапе запроса к банку.
    if (view.effectsEnabled && view.stage == ServiceStage::BankRequest) {
        static const char32_t kSpinner[4] = {U'|', U'/', U'\u2500', U'\\'};
        canvas.put(screenX + kAtmScreenWidth - 1, 3, kSpinner[view.animPhase % 4], Tint::Cyan,
                   /*bold=*/true);
    }

    // --- Индикаторы корпуса: слот карты и лоток купюр -------------------------
    // Подсвечиваются на «своих» этапах: слот — когда карта вставляется или
    // забирается, лоток — на этапах движения наличных. Низкая касса красит
    // лоток красным всегда (символ «денег мало»).
    const int cardX = layout::kAtmX + 2;   // «[▮]» в строке 7 корпуса
    const int trayX = layout::kAtmX + 10;  // «[══]»
    if (view.stage == ServiceStage::InsertCard || view.stage == ServiceStage::ReturnCard) {
        canvas.text(cardX, 7, "[▮]", Tint::Cyan, /*bold=*/true);
    }
    const bool trayActive = view.stage == ServiceStage::CountCash ||
                            view.stage == ServiceStage::DispenseCash ||
                            view.stage == ServiceStage::InsertCash ||
                            view.stage == ServiceStage::VerifyCash;
    if (view.lowCash) {
        canvas.text(trayX, 7, "[══]", Tint::Red, /*bold=*/true);
    } else if (trayActive) {
        canvas.text(trayX, 7, "[══]", Tint::Green, /*bold=*/true);
    }

    // --- Эффект: купюры летят между лотком и клиентом --------------------------
    // Выдача — от лотка к клиенту, внесение — от клиента к лотку. Пара «купюр»
    // (=) со сдвигом фаз, дорожка — узкий зазор между корпусом и человечком.
    if (view.effectsEnabled &&
        (view.stage == ServiceStage::DispenseCash || view.stage == ServiceStage::InsertCash)) {
        const int laneX0 = trayX + 4;                    // сразу за «[══]»
        const int laneLen = layout::kServeX - laneX0;    // до обслуживаемого
        if (laneLen > 1) {
            for (int i = 0; i < 2; ++i) {
                // Сдвиг фаз i*3 взаимно прост с типичной длиной дорожки —
                // купюры не сливаются в одну клетку.
                const int step = (view.animPhase + i * 3) % laneLen;
                const int x = (view.stage == ServiceStage::DispenseCash)
                                  ? laneX0 + step             // из лотка к клиенту
                                  : laneX0 + laneLen - 1 - step;  // от клиента в лоток
                canvas.put(x, 7, U'=', Tint::Green, /*bold=*/true);
            }
        }
    }

    // --- Человечки и подписи --------------------------------------------------
    for (const SceneActorView& a : view.actors) {
        canvas.blit(a.x, a.y, poseRows(a.pose), a.tint, a.bold);
        // Подпись центрируем под спрайтом (спрайт 3 колонки, центр x+1).
        // На нижней дорожке (уходящие) подпись клипается краем канвы — не беда.
        const int labelCols = columnsOf(a.label);
        canvas.text(a.x + 1 - (labelCols - 1) / 2, a.y + 3, a.label, a.labelTint);
        // Терпение на исходе — восклицательный знак над головой.
        if (a.nervous) canvas.put(a.x + 1, a.y - 1, U'!', Tint::Red, /*bold=*/true);
        // Эффект: «пар» злости над уходящим не солоно хлебавши (мерцает в
        // такт шагам — фаза завязана на позицию).
        if (view.effectsEnabled && a.tint == Tint::Red && a.y != layout::kActorTopY &&
            (a.x + view.animPhase) % 2 == 0) {
            canvas.put(a.x + 1, a.y - 1, U'~', Tint::Red);
        }
    }

    // --- Хвост очереди, не влезший на сцену -----------------------------------
    if (view.hiddenQueueCount > 0) {
        canvas.text(view.overflowLabelX, layout::kActorTopY + 1,
                    "… ещё " + std::to_string(view.hiddenQueueCount), Tint::Grey);
    }
}

}  // namespace atmsim::scene
