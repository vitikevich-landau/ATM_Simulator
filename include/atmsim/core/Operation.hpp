#pragma once
// ============================================================================
//  Operation.hpp — правила выполнения банковской операции и запись журнала.
// ============================================================================
#include <chrono>
#include <string>

#include "atmsim/core/Account.hpp"
#include "atmsim/core/Cashbox.hpp"
#include "atmsim/core/Types.hpp"

namespace atmsim {

// Одна строка бизнес-журнала операций (§4.4, §7). Заполняется после выполнения
// операции; администратор видит её через команды operations/export (M4).
struct OperationRecord {
    OperationId id{};
    ClientId clientId{};
    OperationType type{OperationType::CheckBalance};
    Money amount{0};
    Money balanceAfter{0};
    std::chrono::system_clock::time_point timestamp{};  // «настенное» время события
    bool success{false};
    std::string errorMessage;  // пусто, если success == true
};

// Применяет операцию к счёту и кассе по правилам §4.2.
//
// Это ЦЕНТР денежной логики проекта — именно здесь счёт клиента и касса
// банкомата двигаются согласованно, что и обеспечивает инвариант сохранения
// денег. Поэтому реализация (Operation.cpp) снабжена самыми подробными
// комментариями, а на неё завязан property-тест money_conservation_invariant.
//
// Функция не бросает исключений на «не хватило денег» — возвращает
// OperationOutcome со статусом (§6.5).
OperationOutcome applyOperation(OperationType type, Money amount,
                                Account& account, Cashbox& cashbox);

}  // namespace atmsim
