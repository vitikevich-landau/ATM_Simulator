#include "atmsim/console/CommandParser.hpp"
#include "simple_test.hpp"

using namespace atmsim;

TEST(parser_simple_commands) {
    CHECK(parseCommand("status").type == CommandType::Status);
    CHECK(parseCommand("queue").type == CommandType::Queue);
    CHECK(parseCommand("pause").type == CommandType::Pause);
    CHECK(parseCommand("resume").type == CommandType::Resume);
    CHECK(parseCommand("atm").type == CommandType::Atm);
    CHECK(parseCommand("stats").type == CommandType::Stats);
    CHECK(parseCommand("help").type == CommandType::Help);
}

TEST(parser_stop_aliases) {
    CHECK(parseCommand("stop").type == CommandType::Stop);
    CHECK(parseCommand("exit").type == CommandType::Stop);
    CHECK(parseCommand("quit").type == CommandType::Stop);
}

TEST(parser_restart) {
    CHECK(parseCommand("restart").type == CommandType::Restart);
    CHECK(parseCommand("again").type == CommandType::Restart);
}

TEST(parser_empty_and_unknown) {
    CHECK(parseCommand("").type == CommandType::Empty);
    CHECK(parseCommand("   ").type == CommandType::Empty);
    CHECK(parseCommand("frobnicate").type == CommandType::Unknown);
}

TEST(parser_client_and_balance) {
    const Command a = parseCommand("client 42");
    CHECK(a.type == CommandType::Client);
    CHECK(a.error.empty());
    CHECK(a.clientId.has_value());
    CHECK_EQ(static_cast<long long>(*a.clientId), 42LL);

    const Command b = parseCommand("balance 7");
    CHECK(b.type == CommandType::Balance);
    CHECK(b.clientId.has_value());
}

TEST(parser_client_errors) {
    CHECK(!parseCommand("client").error.empty());       // нет номера
    CHECK(!parseCommand("client abc").error.empty());   // не число
    CHECK(!parseCommand("client 0").error.empty());     // не положительное
    CHECK(!parseCommand("client -3").error.empty());
}

TEST(parser_operations_flags) {
    const Command c = parseCommand("operations --client 5 --last 10 --type withdraw");
    CHECK(c.type == CommandType::Operations);
    CHECK(c.error.empty());
    CHECK(c.clientId.has_value());
    CHECK_EQ(static_cast<long long>(*c.clientId), 5LL);
    CHECK(c.last.has_value());
    CHECK_EQ(static_cast<long long>(*c.last), 10LL);
    CHECK(c.opType.has_value());
    CHECK(*c.opType == OperationType::Withdraw);
}

TEST(parser_operations_errors) {
    CHECK(!parseCommand("operations --last").error.empty());       // нет значения
    CHECK(!parseCommand("operations --type frobbing").error.empty()); // неизвестный тип
    CHECK(!parseCommand("operations --wat 3").error.empty());      // неизвестный флаг
}

TEST(parser_export) {
    const Command c = parseCommand("export out.csv");
    CHECK(c.type == CommandType::Export);
    CHECK_EQ(c.filename, std::string("out.csv"));
    CHECK(!parseCommand("export").error.empty());  // нет имени файла
}

TEST(parser_maintenance) {
    CHECK(parseCommand("maintenance stop").type == CommandType::MaintenanceStop);

    const Command a = parseCommand("maintenance start");
    CHECK(a.type == CommandType::MaintenanceStart);
    CHECK(!a.seconds.has_value());  // без длительности

    const Command b = parseCommand("maintenance start 120");
    CHECK(b.type == CommandType::MaintenanceStart);
    CHECK(b.seconds.has_value());
    CHECK_EQ(*b.seconds, 120);
}

TEST(parser_maintenance_errors) {
    CHECK(!parseCommand("maintenance").error.empty());        // нет start/stop
    CHECK(!parseCommand("maintenance frob").error.empty());   // не start/stop
    CHECK(!parseCommand("maintenance start abc").error.empty()); // длительность не число
    // Длительность больше INT_MAX должна отвергаться, а не тихо переполняться в
    // <= 0 (что движок принял бы за бессрочное ТО). 4294967296 = 2^32.
    CHECK(!parseCommand("maintenance start 4294967296").error.empty());
}

TEST(parser_live) {
    CHECK(parseCommand("live").type == CommandType::Live);
    CHECK(parseCommand("live off").type == CommandType::LiveOff);
}
