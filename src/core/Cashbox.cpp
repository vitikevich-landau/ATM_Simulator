#include "atmsim/core/Cashbox.hpp"

#include <limits>

namespace atmsim {

Cashbox::Cashbox(Money initialAmount) : balance_(initialAmount) {}

void Cashbox::dispense(Money amount) {
    // Предусловие — canDispense(amount) уже истинно (проверяет вызывающий).
    // Здесь только само движение денег.
    balance_ -= amount;
}

bool Cashbox::canAccept(Money amount) const {
    return amount > 0 && balance_ <= std::numeric_limits<Money>::max() - amount;
}

bool Cashbox::accept(Money amount) {
    if (!canAccept(amount)) {
        return false;
    }
    balance_ += amount;
    return true;
}

}  // namespace atmsim
