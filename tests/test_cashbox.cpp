#include "atmsim/core/Cashbox.hpp"
#include "simple_test.hpp"

using namespace atmsim;

TEST(cashbox_accept_and_dispense) {
    Cashbox box(100000);
    box.accept(50000);
    CHECK_EQ(box.balance(), Money(150000));

    CHECK(box.canDispense(150000));
    box.dispense(150000);
    CHECK_EQ(box.balance(), Money(0));
}

TEST(cashbox_cannot_dispense_more_than_it_has) {
    Cashbox box(1000);
    CHECK(box.canDispense(1000));    // ровно столько — можно
    CHECK(!box.canDispense(1001));   // больше остатка — нельзя
    CHECK(!box.canDispense(0));      // ноль/отрицатель — нельзя
    CHECK(!box.canDispense(-5));
}
