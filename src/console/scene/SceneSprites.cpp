#include "atmsim/console/scene/SceneSprites.hpp"

namespace atmsim::scene {

const std::vector<std::string>& poseRows(ActorPose pose) {
    // Человечек намеренно прост (3×3): на такой площади читаются и поза, и
    // движение, а очередь из 8–10 фигур влезает в 100 колонок вместе с
    // подписями. Пробелы в строках ПРОЗРАЧНЫ при blit (фон сохраняется).
    static const std::vector<std::string> kStand = {
        " o ",
        "/|\\",
        "/ \\",
    };
    // Рука вытянута влево, к банкомату (клиент стоит справа от корпуса).
    static const std::vector<std::string> kReachLeft = {
        " o ",
        "-|\\",
        "/ \\",
    };
    // Два кадра походки: ноги врозь / ноги вместе. Чередование кадров идёт по
    // ПОЗИЦИИ актёра (см. ScenePresenter), поэтому темп шагов совпадает со
    // скоростью движения.
    static const std::vector<std::string> kWalkA = {
        " o ",
        "/|\\",
        "/ \\",
    };
    static const std::vector<std::string> kWalkB = {
        " o ",
        "/|\\",
        " | ",
    };
    // Idle-вариации: перенос веса с руки на пояс (скобка = согнутая рука).
    static const std::vector<std::string> kIdleShiftL = {
        " o ",
        "(|\\",
        "/ \\",
    };
    static const std::vector<std::string> kIdleShiftR = {
        " o ",
        "/|)",
        "/ \\",
    };
    // Взмах рукой (фиджет) и ликование (руки вверх).
    static const std::vector<std::string> kWave = {
        " o/",
        "/| ",
        "/ \\",
    };
    static const std::vector<std::string> kCheer = {
        "\\o/",
        " | ",
        "/ \\",
    };
    switch (pose) {
        case ActorPose::ReachLeft: return kReachLeft;
        case ActorPose::WalkA: return kWalkA;
        case ActorPose::WalkB: return kWalkB;
        case ActorPose::IdleShiftL: return kIdleShiftL;
        case ActorPose::IdleShiftR: return kIdleShiftR;
        case ActorPose::Wave: return kWave;
        case ActorPose::Cheer: return kCheer;
        case ActorPose::Stand: break;
    }
    return kStand;
}

const std::vector<std::string>& atmArtRows() {
    // Корпус 16×9. Внутренняя рамка — экран 12 колонок (kAtmScreenWidth),
    // его содержимое SceneComposer рисует поверх пустых строк. В нижнем ряду
    // корпуса — слот карты [▮] и лоток выдачи/приёма [══]; их подсветка по
    // этапу обслуживания тоже дело композера.
    static const std::vector<std::string> kAtm = {
        "┌──────────────┐",
        "│   БАНКОМАТ   │",
        "│┌────────────┐│",
        "││            ││",
        "││            ││",
        "││            ││",
        "│└────────────┘│",
        "│ [▮]     [══] │",
        "└──────────────┘",
    };
    return kAtm;
}

std::array<std::string, 2> screenStageText(ServiceStage stage) {
    // Реплики от лица банкомата, каждая строка <= 12 колонок (kAtmScreenWidth,
    // закреплено тестом). Этапы клиента — повелительное наклонение («ВСТАВЬТЕ
    // КАРТУ»), этапы самого банкомата — процесс («ОТСЧЁТ КУПЮР»).
    switch (stage) {
        case ServiceStage::InsertCard:      return {"ВСТАВЬТЕ", "КАРТУ"};
        case ServiceStage::EnterPin:        return {"ВВЕДИТЕ", "PIN-КОД"};
        case ServiceStage::SelectOperation: return {"ВЫБЕРИТЕ", "ОПЕРАЦИЮ"};
        case ServiceStage::EnterAmount:     return {"ВВЕДИТЕ", "СУММУ"};
        case ServiceStage::BankRequest:     return {"СВЯЗЬ С", "БАНКОМ..."};
        case ServiceStage::CountCash:       return {"ОТСЧЁТ", "КУПЮР..."};
        case ServiceStage::DispenseCash:    return {"ЗАБЕРИТЕ", "НАЛИЧНЫЕ"};
        case ServiceStage::InsertCash:      return {"ВЛОЖИТЕ", "КУПЮРЫ"};
        case ServiceStage::VerifyCash:      return {"ПЕРЕСЧЁТ", "КУПЮР..."};
        case ServiceStage::ShowBalance:     return {"ВАШ", "БАЛАНС"};
        case ServiceStage::PrintReceipt:    return {"ПЕЧАТЬ", "ЧЕКА..."};
        case ServiceStage::ReturnCard:      return {"ЗАБЕРИТЕ", "КАРТУ"};
    }
    return {"", ""};
}

}  // namespace atmsim::scene
