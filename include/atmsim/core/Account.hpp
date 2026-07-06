#pragma once
// ============================================================================
//  Account.hpp — банковский счёт одного клиента.
// ============================================================================
#include "atmsim/core/Money.hpp"
#include "atmsim/core/Types.hpp"

namespace atmsim {

// Счёт хранит только баланс (в минорных единицах) и свой идентификатор.
//
// О ПОТОКОБЕЗОПАСНОСТИ (важно): на этапе M1 здесь СОЗНАТЕЛЬНО нет mutex.
//   * §13 ТЗ предписывает сначала отладить доменное ядро без потоков, а
//     многопоточность добавлять поверх проверенного кода (M3).
//   * Отсутствие mutex делает Account копируемым/перемещаемым — его можно
//     спокойно хранить в std::vector. Класс с std::mutex так не умеет.
//   * По модели «один писатель» (§6.1) счета мутирует только поток Engine, а
//     читатели работают через снимки — поэтому, возможно, отдельный mutex на
//     счёт вообще не понадобится. Это решение примем осознанно на M3.
class Account {
public:
    Account(AccountId id, Money initialBalance);

    AccountId id() const { return id_; }
    Money balance() const { return balance_; }

    // Списание со счёта. Возможные исходы:
    //   Success            — списано, баланс уменьшен;
    //   InsufficientFunds  — на счёте меньше запрошенного (баланс не изменён);
    //   InvalidAmount      — сумма <= 0 (баланс не изменён).
    OperationOutcome withdraw(Money amount);

    // Пополнение счёта. Исходы: Success либо InvalidAmount (сумма <= 0).
    OperationOutcome deposit(Money amount);

private:
    AccountId id_;
    Money balance_;
};

}  // namespace atmsim
