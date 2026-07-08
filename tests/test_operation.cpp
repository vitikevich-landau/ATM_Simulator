#include <cstdint>
#include <limits>
#include <vector>

#include "atmsim/core/Client.hpp"
#include "atmsim/core/Operation.hpp"
#include "simple_test.hpp"

using namespace atmsim;

namespace {
// Сумма балансов всех счетов — понадобится для инварианта сохранения денег.
Money sumAccounts(const std::vector<Account>& accounts) {
    Money total = 0;
    for (const auto& a : accounts) total += a.balance();
    return total;
}
}  // namespace

TEST(operation_checkbalance_changes_nothing) {
    Account a(1, 100000);
    Cashbox box(500000);
    const auto r = applyOperation(OperationType::CheckBalance, 0, a, box);
    CHECK(r.ok());
    CHECK_EQ(a.balance(), Money(100000));
    CHECK_EQ(box.balance(), Money(500000));
}

TEST(operation_deposit_moves_account_and_cashbox_together) {
    Account a(1, 100000);
    Cashbox box(500000);
    const auto r = applyOperation(OperationType::Deposit, 20000, a, box);
    CHECK(r.ok());
    CHECK_EQ(a.balance(), Money(120000));    // счёт вырос
    CHECK_EQ(box.balance(), Money(520000));  // и касса выросла на ту же сумму
}

TEST(operation_withdraw_double_check_funds_and_cash) {
    // Случай 1: на счёте денег вагон, но в кассе пусто -> InsufficientCash.
    Account rich(1, 1000000);
    Cashbox small(10000);
    const auto r1 = applyOperation(OperationType::Withdraw, 20000, rich, small);
    CHECK(r1.status == OperationStatus::InsufficientCash);
    CHECK_EQ(rich.balance(), Money(1000000));  // ничего не списано
    CHECK_EQ(small.balance(), Money(10000));

    // Случай 2: касса полна, но на счёте не хватает -> InsufficientFunds.
    Account poor(2, 5000);
    Cashbox big(1000000);
    const auto r2 = applyOperation(OperationType::Withdraw, 20000, poor, big);
    CHECK(r2.status == OperationStatus::InsufficientFunds);
    CHECK_EQ(poor.balance(), Money(5000));
    CHECK_EQ(big.balance(), Money(1000000));

    // Случай 3: всего хватает -> успех, обе стороны уменьшились на сумму.
    Account ok(3, 100000);
    Cashbox full(100000);
    const auto r3 = applyOperation(OperationType::Withdraw, 30000, ok, full);
    CHECK(r3.ok());
    CHECK_EQ(ok.balance(), Money(70000));
    CHECK_EQ(full.balance(), Money(70000));
}

TEST(operation_deposit_overflow_leaves_account_and_cashbox_unchanged) {
    {
        Account a(1, std::numeric_limits<Money>::max() - 5);
        Cashbox box(1000);
        const auto r = applyOperation(OperationType::Deposit, 10, a, box);
        CHECK(r.status == OperationStatus::Overflow);
        CHECK_EQ(a.balance(), std::numeric_limits<Money>::max() - 5);
        CHECK_EQ(box.balance(), Money(1000));
    }
    {
        Account a(1, 1000);
        Cashbox box(std::numeric_limits<Money>::max() - 5);
        const auto r = applyOperation(OperationType::Deposit, 10, a, box);
        CHECK(r.status == OperationStatus::Overflow);
        CHECK_EQ(a.balance(), Money(1000));
        CHECK_EQ(box.balance(), std::numeric_limits<Money>::max() - 5);
    }
}

// ГЛАВНЫЙ тест ядра: инвариант сохранения денег (§4.2, §11).
// Прогоняем длинную ДЕТЕРМИНИРОВАННУЮ последовательность операций и проверяем,
// что разность (сумма счетов − касса) не изменилась ни на копейку.
TEST(money_conservation_invariant) {
    std::vector<Account> accounts;
    accounts.reserve(5);
    for (int i = 0; i < 5; ++i) {
        accounts.emplace_back(static_cast<AccountId>(i), 100000);  // по 1000.00
    }
    Cashbox box(500000);  // 5000.00

    const Money accountsBefore = sumAccounts(accounts);
    const Money cashBefore = box.balance();

    // Суммируем только УСПЕШНЫЕ движения денег.
    Money appliedDeposits = 0;
    Money appliedWithdrawals = 0;

    // Свой линейный конгруэнтный генератор (LCG): детерминированный и не зависит
    // от реализации <random> — тот же seed всегда даёт тот же прогон (§5).
    std::uint64_t rng = 123456789ull;
    auto next = [&rng]() -> std::uint64_t {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return rng >> 33;  // старшие биты «качественнее» младших у LCG
    };

    for (int step = 0; step < 2000; ++step) {
        Account& acct = accounts[next() % accounts.size()];
        const std::uint64_t pick = next() % 3;
        const Money amount = static_cast<Money>(next() % 50000);  // 0..499.99

        OperationType op = OperationType::CheckBalance;
        if (pick == 1) op = OperationType::Deposit;
        else if (pick == 2) op = OperationType::Withdraw;

        const auto r = applyOperation(op, amount, acct, box);
        if (r.ok()) {
            if (op == OperationType::Deposit) appliedDeposits += amount;
            else if (op == OperationType::Withdraw) appliedWithdrawals += amount;
        }
    }

    const Money delta = appliedDeposits - appliedWithdrawals;
    // Касса изменилась ровно на (внесения − снятия).
    CHECK_EQ(box.balance() - cashBefore, delta);
    // Сумма счетов изменилась на ту же величину.
    CHECK_EQ(sumAccounts(accounts) - accountsBefore, delta);
    // Следствие — главный инвариант: (сумма счетов − касса) не изменилась.
    CHECK_EQ(sumAccounts(accounts) - box.balance(), accountsBefore - cashBefore);
}

TEST(client_struct_has_sane_defaults) {
    Client c;
    CHECK(c.state == ClientState::Waiting);
    CHECK(c.requestedOperation == OperationType::CheckBalance);
    CHECK_EQ(c.amount, Money(0));
}
