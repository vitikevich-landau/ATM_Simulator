#include "atmsim/core/Cashbox.hpp"

namespace atmsim {

Cashbox::Cashbox(Money initialAmount) : balance_(initialAmount) {}

void Cashbox::dispense(Money amount) {
    // Предусловие — canDispense(amount) уже истинно (проверяет вызывающий).
    // Здесь только само движение денег.
    balance_ -= amount;
}

void Cashbox::accept(Money amount) {
    balance_ += amount;
}

}  // namespace atmsim
