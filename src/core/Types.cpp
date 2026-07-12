#include "atmsim/core/Types.hpp"

#include <cassert>

namespace atmsim {

// Примечание про хвост каждого to_string ниже: switch покрывает ВСЕ значения
// enum (за полнотой следит -Wswitch), поэтому фактически он недостижим. Но
// функция не void, а значение enum технически можно испортить (cast из мусора/
// повреждение памяти). Раньше здесь молча возвращалось "Unknown" — теперь в
// debug/тестах падаем ассертом (баг виден громко), в release оставляем "Unknown"
// как безопасную деградацию, а не аварию.

std::string to_string(OperationType t) {
    switch (t) {
        case OperationType::CheckBalance: return "CheckBalance";
        case OperationType::Withdraw:     return "Withdraw";
        case OperationType::Deposit:      return "Deposit";
    }
    assert(false && "неизвестное значение OperationType в to_string");
    return "Unknown";
}

std::string to_string(ClientState s) {
    switch (s) {
        case ClientState::Waiting:   return "Waiting";
        case ClientState::InService: return "InService";
        case ClientState::Served:    return "Served";
        case ClientState::LeftQueue: return "LeftQueue";
    }
    assert(false && "неизвестное значение ClientState в to_string");
    return "Unknown";
}

std::string to_string(AtmState s) {
    switch (s) {
        case AtmState::Idle:        return "Idle";
        case AtmState::Serving:     return "Serving";
        case AtmState::Paused:      return "Paused";
        case AtmState::Maintenance: return "Maintenance";
        case AtmState::Stopped:     return "Stopped";
    }
    assert(false && "неизвестное значение AtmState в to_string");
    return "Unknown";
}

std::string to_string(OperationStatus s) {
    switch (s) {
        case OperationStatus::Success:           return "Success";
        case OperationStatus::InsufficientFunds: return "InsufficientFunds";
        case OperationStatus::InsufficientCash:  return "InsufficientCash";
        case OperationStatus::InvalidAmount:     return "InvalidAmount";
        case OperationStatus::Overflow:          return "Overflow";
    }
    assert(false && "неизвестное значение OperationStatus в to_string");
    return "Unknown";
}

}  // namespace atmsim
