#pragma once

#include <cmath>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {

struct Case {
    std::string name;
    std::function<void()> function;
};
inline std::vector<Case>& cases() {
    static std::vector<Case> value;
    return value;
}

struct Registration {
    Registration(std::string name, std::function<void()> function) {
        cases().push_back({std::move(name), std::move(function)});
    }
};

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (condition)
        return;
    std::ostringstream message;
    message << file << ':' << line << ": requirement failed: " << expression;
    throw std::runtime_error(message.str());
}

inline void requireNear(double actual, double expected, double tolerance, const char* file, int line) {
    if (std::abs(actual - expected) <= tolerance)
        return;
    std::ostringstream message;
    message << file << ':' << line << ": expected " << expected << " +/- " << tolerance << ", got " << actual;
    throw std::runtime_error(message.str());
}

} // namespace test

#define SMP_JOIN_INNER(a, b) a##b
#define SMP_JOIN(a, b) SMP_JOIN_INNER(a, b)
#define TEST_CASE(name)                                                                                                \
    static void SMP_JOIN(test_function_, __LINE__)();                                                                  \
    static test::Registration SMP_JOIN(test_registration_, __LINE__)(name, SMP_JOIN(test_function_, __LINE__));        \
    static void SMP_JOIN(test_function_, __LINE__)()
#define REQUIRE(expression) test::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define REQUIRE_NEAR(actual, expected, tolerance)                                                                      \
    test::requireNear((actual), (expected), (tolerance), __FILE__, __LINE__)
