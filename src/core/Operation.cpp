#include "atmsim/core/Operation.hpp"

namespace atmsim {

// ----------------------------------------------------------------------------
//  applyOperation — центр денежной логики. Здесь счёт клиента и касса банкомата
//  двигаются СОГЛАСОВАННО, и именно это обеспечивает инвариант сохранения денег.
//
//  Ключевая идея инварианта (проверяется тестом money_conservation_invariant):
//    * Внесение:  счёт += amount  И  касса += amount;
//    * Снятие:    счёт -= amount  И  касса -= amount.
//  Обе величины всегда меняются на одну и ту же сумму в одну сторону, поэтому
//  разность (сумма всех счетов − касса) НЕ меняется никогда. Деньги не берутся
//  из ниоткуда и не исчезают (§4.2).
// ----------------------------------------------------------------------------
OperationOutcome applyOperation(OperationType type, Money amount,
                                Account& account, Cashbox& cashbox) {
    switch (type) {
        case OperationType::CheckBalance:
            // Проверка баланса ничего не двигает — ни счёт, ни кассу.
            return {OperationStatus::Success, account.balance()};

        case OperationType::Deposit: {
            if (amount <= 0) {
                return {OperationStatus::InvalidAmount, account.balance()};
            }
            // Деньги входят В систему: счёт и касса растут на одну сумму.
            account.deposit(amount);
            cashbox.accept(amount);
            return {OperationStatus::Success, account.balance()};
        }

        case OperationType::Withdraw: {
            if (amount <= 0) {
                return {OperationStatus::InvalidAmount, account.balance()};
            }
            // ДВОЙНАЯ ПРОВЕРКА (§4.2): снятие возможно, только если денег хватает
            // И на счёте клиента, И в кассе банкомата. Проверяем счёт первым,
            // чтобы причина отказа была осмысленной (сначала «твои» деньги).
            if (amount > account.balance()) {
                return {OperationStatus::InsufficientFunds, account.balance()};
            }
            if (!cashbox.canDispense(amount)) {
                return {OperationStatus::InsufficientCash, account.balance()};
            }
            // Обе стороны уменьшаются на одну сумму: деньги выходят ИЗ системы.
            account.withdraw(amount);
            cashbox.dispense(amount);
            return {OperationStatus::Success, account.balance()};
        }
    }

    // Недостижимо при корректном enum, но компилятор требует возврата на всех
    // путях. Возвращаем «невалидную сумму» как самый безопасный нейтральный исход.
    return {OperationStatus::InvalidAmount, account.balance()};
}

}  // namespace atmsim
