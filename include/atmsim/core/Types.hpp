#pragma once
// ============================================================================
//  Types.hpp — базовые типы предметной области: идентификаторы, перечисления
//  состояний и результат операции. Собраны в одном месте, чтобы разорвать
//  циклические зависимости между заголовками (Account <-> Operation).
// ============================================================================
#include <cstdint>
#include <string>

#include "atmsim/core/Money.hpp"

namespace atmsim {

// Именованные идентификаторы. Это всего лишь псевдонимы для целых, но название
// делает сигнатуры читаемее: clientReport(ClientId) понятнее, чем clientReport(uint64_t).
using ClientId    = std::uint64_t;
using AccountId   = std::uint64_t;
using OperationId = std::uint64_t;

// Тип банковской операции. На M1 — три обязательные (§4.2); расширения
// (MiniStatement, Transfer, ChangePin) добавятся позже, если понадобятся.
enum class OperationType { CheckBalance, Withdraw, Deposit };

// Состояние клиента в системе (§7).
enum class ClientState {
    Waiting,    // стоит в очереди
    InService,  // обслуживается прямо сейчас
    Served,     // обслуживание успешно завершено
    LeftQueue   // ушёл, не дождавшись (истекло терпение или началось ТО)
};

// Состояние банкомата (§7). Понадобится начиная с M3 (потоки/режимы).
enum class AtmState { Idle, Serving, Paused, Maintenance, Stopped };

// Исход операции. Это НЕ ошибки-исключения, а штатные бизнес-результаты (§6.5):
// «не хватило денег» — обычный, ожидаемый ответ, а не аварийная ситуация.
// Overflow — тоже штатный отказ: операция не помещается в Money без переполнения.
enum class OperationStatus { Success, InsufficientFunds, InsufficientCash, InvalidAmount, Overflow };

// Результат применения операции (§6.3): статус + баланс счёта после операции.
struct OperationOutcome {
    OperationStatus status{OperationStatus::Success};
    Money balanceAfter{0};

    // Небольшой помощник для читаемости: outcome.ok() вместо сравнения с enum.
    bool ok() const { return status == OperationStatus::Success; }
};

// Человекочитаемые имена перечислений — для журналов, отчётов и сообщений тестов.
std::string to_string(OperationType t);
std::string to_string(ClientState s);
std::string to_string(AtmState s);
std::string to_string(OperationStatus s);

}  // namespace atmsim
