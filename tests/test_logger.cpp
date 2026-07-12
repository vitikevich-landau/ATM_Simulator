#include <fstream>
#include <iterator>
#include <string>

#include "atmsim/reporting/Logger.hpp"
#include "simple_test.hpp"

using namespace atmsim;

namespace {
std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

// Логгер отбрасывает сообщения ниже минимального уровня и помечает уровни.
TEST(logger_respects_min_level) {
    const std::string path = "build/test_logger.log";
    {
        Logger lg(path, LogLevel::Info);
        lg.debug("DEBUG_MSG");   // ниже Info -> не должно попасть в файл
        lg.info("INFO_MSG");
        lg.warn("WARN_MSG");
        lg.error("ERROR_MSG");
    }  // деструктор закрывает файл
    const std::string all = readFile(path);
    CHECK(all.find("DEBUG_MSG") == std::string::npos);
    CHECK(all.find("INFO_MSG") != std::string::npos);
    CHECK(all.find("WARN_MSG") != std::string::npos);
    CHECK(all.find("ERROR_MSG") != std::string::npos);
    CHECK(all.find("[INFO]") != std::string::npos);
    CHECK(all.find("[WARN]") != std::string::npos);   // метка печати уровня Warn
    CHECK(all.find("[ERROR]") != std::string::npos);
}

TEST(logger_parse_level) {
    CHECK(Logger::parseLevel("debug") == LogLevel::Debug);
    CHECK(Logger::parseLevel("info") == LogLevel::Info);
    CHECK(Logger::parseLevel("warn") == LogLevel::Warn);
    CHECK(Logger::parseLevel("error") == LogLevel::Error);
    CHECK(Logger::parseLevel("nonsense") == LogLevel::Info);  // безопасный дефолт
}

// tryParseLevel отличает неизвестный уровень (nullopt) от валидного и принимает
// входной алиас "warning". Обе половины (разбор/печать) читают одну таблицу.
TEST(logger_try_parse_level) {
    CHECK(Logger::tryParseLevel("debug") == LogLevel::Debug);
    CHECK(Logger::tryParseLevel("info") == LogLevel::Info);
    CHECK(Logger::tryParseLevel("warn") == LogLevel::Warn);
    CHECK(Logger::tryParseLevel("warning") == LogLevel::Warn);  // входной алиас
    CHECK(Logger::tryParseLevel("error") == LogLevel::Error);
    CHECK(!Logger::tryParseLevel("nonsense").has_value());       // неизвестное -> nullopt
}
