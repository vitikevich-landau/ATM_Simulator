#include "atmsim/console/scene/ActorAnim.hpp"

#include <cmath>

namespace atmsim::scene {
namespace {

// Личная фаза актёра, 0..4 с: сдвигает все его циклы, чтобы очередь не
// двигалась синхронно, как кордебалет.
double personalPhase(ClientId id) {
    return static_cast<double>(splitmix64(static_cast<std::uint64_t>(id)) % 4000) / 1000.0;
}

}  // namespace

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

ActorPose pickIdlePose(ClientId id, double tSec) {
    const double tp = tSec + personalPhase(id);
    // Редкий взмах: время делится на окна по 8 с; хэш (id, номер окна)
    // выбирает «повезло ли этому актёру в этом окне» (шанс 1/5 -> в среднем
    // раз в 40 с), взмах длится первую секунду окна.
    const std::uint64_t window = static_cast<std::uint64_t>(tp / 8.0);
    const std::uint64_t luck =
        splitmix64(static_cast<std::uint64_t>(id) ^ (window * 0x9E3779B97F4A7C15ull));
    if (luck % 5 == 0 && tp - static_cast<double>(window) * 8.0 < 1.0) {
        return ActorPose::Wave;
    }
    // Неспешное переминание: Stand -> ShiftL -> Stand -> ShiftR, шаг ~2.2 с.
    switch (static_cast<long long>(tp / 2.2) % 4) {
        case 1: return ActorPose::IdleShiftL;
        case 3: return ActorPose::IdleShiftR;
        default: return ActorPose::Stand;
    }
}

ActorPose pickNervousPose(ClientId id, double tSec) {
    // Терпение на исходе: переступает с ноги на ногу трижды в секунду.
    const double tp = tSec + personalPhase(id);
    return (static_cast<long long>(tp * 3.0) % 2 == 0) ? ActorPose::IdleShiftL
                                                       : ActorPose::IdleShiftR;
}

ActorPose pickActPose(ClientId id, std::optional<ServiceStage> stage, double tSec) {
    if (!stage) return ActorPose::ReachLeft;
    switch (*stage) {
        case ServiceStage::InsertCard:
        case ServiceStage::EnterPin:
        case ServiceStage::SelectOperation:
        case ServiceStage::EnterAmount:
        case ServiceStage::DispenseCash:
        case ServiceStage::InsertCash:
        case ServiceStage::ReturnCard:
            // «Ручной» этап: рука ходит к панели и обратно (~2 Гц) — клиент
            // жмёт кнопки, вкладывает купюры, забирает карту.
            return (static_cast<long long>(tSec * 2.0) % 2 == 0) ? ActorPose::ReachLeft
                                                                 : ActorPose::Stand;
        case ServiceStage::BankRequest:
        case ServiceStage::CountCash:
        case ServiceStage::VerifyCash:
        case ServiceStage::ShowBalance:
        case ServiceStage::PrintReceipt:
            break;
    }
    // «Машинный» этап: работает банкомат, клиент ждёт и переминается.
    return pickIdlePose(id, tSec);
}

}  // namespace atmsim::scene
