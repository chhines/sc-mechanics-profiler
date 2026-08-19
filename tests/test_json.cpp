#include "test_framework.h"

#include "util/json.h"

#include <limits>
#include <string>

namespace {

bool parseThrows(const std::string& text) {
    try {
        (void)smp::json::parse(text);
        return false;
    } catch (...) {
        return true;
    }
}

bool stringifyThrows(const smp::json::Value& value) {
    try {
        (void)smp::json::stringify(value);
        return false;
    } catch (...) {
        return true;
    }
}

std::string nestedArrays(std::size_t depth) {
    return std::string(depth, '[') + "0" + std::string(depth, ']');
}

} // namespace

TEST_CASE("json parser caps nesting depth") {
    REQUIRE(!parseThrows(nestedArrays(256)));
    REQUIRE(parseThrows(nestedArrays(257)));
}

TEST_CASE("json parser rejects malformed numbers") {
    REQUIRE(parseThrows("01"));
    REQUIRE(parseThrows("-01"));
    REQUIRE(parseThrows("1."));
    REQUIRE(parseThrows("1e"));
    REQUIRE(parseThrows("1e+"));
    REQUIRE(parseThrows("-"));
}

TEST_CASE("json parser combines unicode surrogate pairs") {
    const auto value = smp::json::parse("\"\\uD83D\\uDE00\"");
    REQUIRE(value.asString() == std::string("\xF0\x9F\x98\x80", 4));
    REQUIRE(parseThrows("\"\\uD83D\""));
    REQUIRE(parseThrows("\"\\uDE00\""));
}

TEST_CASE("json duplicate object keys use the last value") {
    const auto value = smp::json::parse("{\"answer\": 1, \"answer\": 2}");
    REQUIRE(value["answer"].asInt() == 2);
}

TEST_CASE("json integer conversion rejects values outside int range") {
    const auto value = smp::json::parse("1e300");
    REQUIRE(value.asInt(1234) == 1234);
}

TEST_CASE("json parser rejects unescaped control characters") {
    std::string text{"\"a"};
    text.push_back('\x01');
    text += "b\"";
    REQUIRE(parseThrows(text));
}

TEST_CASE("json serializer rejects non-finite numbers") {
    REQUIRE(stringifyThrows(smp::json::Value(std::numeric_limits<double>::infinity())));
}
