#include "atmsim/core/Money.hpp"
#include "simple_test.hpp"

using namespace atmsim;

TEST(money_format_positive) {
    CHECK_EQ(formatMoney(50000), std::string("500.00"));
    CHECK_EQ(formatMoney(134000), std::string("1340.00"));
    CHECK_EQ(formatMoney(5), std::string("0.05"));
    CHECK_EQ(formatMoney(0), std::string("0.00"));
    CHECK_EQ(formatMoney(1), std::string("0.01"));
}

TEST(money_format_negative) {
    CHECK_EQ(formatMoney(-5), std::string("-0.05"));
    CHECK_EQ(formatMoney(-134000), std::string("-1340.00"));
}
