#include "atmsim/core/Account.hpp"

namespace atmsim {

Account::Account(AccountId id, Money initialBalance)
    : id_(id), balance_(initialBalance) {}

OperationOutcome Account::withdraw(Money amount) {
    // Сумма должна быть положительной. Отрицательное «снятие» было бы скрытым
    // пополнением — запрещаем явно.
    if (amount <= 0) {
        return {OperationStatus::InvalidAmount, balance_};
    }
    // Нельзя уйти в минус: на счёте должно хватать средств.
    if (amount > balance_) {
        return {OperationStatus::InsufficientFunds, balance_};
    }
    balance_ -= amount;
    return {OperationStatus::Success, balance_};
}

OperationOutcome Account::deposit(Money amount) {
    if (amount <= 0) {
        return {OperationStatus::InvalidAmount, balance_};
    }
    balance_ += amount;
    return {OperationStatus::Success, balance_};
}

}  // namespace atmsim
