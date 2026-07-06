#include "atmsim/core/Account.hpp"
#include "simple_test.hpp"

using namespace atmsim;

TEST(account_deposit_increases_balance) {
    Account a(1, 100000);            // 1000.00
    const auto r = a.deposit(50000); //  500.00
    CHECK(r.ok());
    CHECK_EQ(a.balance(), Money(150000));
    CHECK_EQ(r.balanceAfter, Money(150000));
}

TEST(account_withdraw_reduces_balance) {
    Account a(1, 100000);
    const auto r = a.withdraw(30000);
    CHECK(r.ok());
    CHECK_EQ(a.balance(), Money(70000));
    CHECK_EQ(r.balanceAfter, Money(70000));
}

TEST(account_withdraw_insufficient_funds_leaves_balance) {
    Account a(1, 10000);
    const auto r = a.withdraw(20000);
    CHECK(r.status == OperationStatus::InsufficientFunds);
    CHECK(!r.ok());
    CHECK_EQ(a.balance(), Money(10000));  // баланс не тронут
}

TEST(account_rejects_nonpositive_amount) {
    Account a(1, 10000);
    CHECK(a.withdraw(0).status == OperationStatus::InvalidAmount);
    CHECK(a.withdraw(-100).status == OperationStatus::InvalidAmount);
    CHECK(a.deposit(0).status == OperationStatus::InvalidAmount);
    CHECK(a.deposit(-100).status == OperationStatus::InvalidAmount);
    CHECK_EQ(a.balance(), Money(10000));  // ни одна из попыток не изменила баланс
}
