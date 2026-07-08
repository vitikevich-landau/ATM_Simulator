#include "atmsim/core/Types.hpp"

namespace atmsim {

std::string to_string(OperationType t) {
    switch (t) {
        case OperationType::CheckBalance: return "CheckBalance";
        case OperationType::Withdraw:     return "Withdraw";
        case OperationType::Deposit:      return "Deposit";
    }
    return "Unknown";
}

std::string to_string(ClientState s) {
    switch (s) {
        case ClientState::Waiting:   return "Waiting";
        case ClientState::InService: return "InService";
        case ClientState::Served:    return "Served";
        case ClientState::LeftQueue: return "LeftQueue";
    }
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
    return "Unknown";
}

}  // namespace atmsim
