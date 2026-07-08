#pragma once
// ============================================================================
//  Cashbox.hpp — касса банкомата (сколько ФИЗИЧЕСКОЙ наличности внутри).
// ============================================================================
#include "atmsim/core/Money.hpp"

namespace atmsim {

// Касса — это наличные в самом устройстве. Она ограничивает снятие независимо
// от баланса счёта: даже если у клиента на счёте миллион, банкомат не выдаст
// больше, чем в нём физически есть (двойная проверка снятия, §4.2).
//
// О ПОТОКОБЕЗОПАСНОСТИ: как и Account, на M1 без atomic — потоки на M3.
// В §6.3 предлагается std::atomic<Money>; вернёмся к этому вместе с Engine.
class Cashbox {
public:
    explicit Cashbox(Money initialAmount);

    Money balance() const { return balance_; }

    // Хватает ли наличности, чтобы выдать amount. Заодно отсекает amount <= 0.
    bool canDispense(Money amount) const { return amount > 0 && amount <= balance_; }
    bool canAccept(Money amount) const;

    // Выдать наличные (уменьшить кассу).
    // ПРЕДУСЛОВИЕ: вызывающий уже проверил canDispense(amount). Метод не
    // перепроверяет, чтобы не «прятать» ошибку логики — её должен ловить
    // вызывающий (в бизнес-логике это делает applyOperation, см. Operation.hpp).
    void dispense(Money amount);

    // Принять наличные (увеличить кассу) — при внесении клиентом.
    // Возвращает false, если сумма невалидна или привела бы к overflow.
    bool accept(Money amount);

private:
    Money balance_;
};

}  // namespace atmsim
