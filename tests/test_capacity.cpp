#include "test_framework.h"

#include "analysis/capacity.h"

TEST_CASE("capacity breakpoint uses the first two-metric 20 percent degradation") {
    const std::vector<scm::CapacityBinInput> bins{{0, {100.0, 120.0, std::nullopt}},
                                                  {100, {110.0, 130.0, 90.0}},
                                                  {150, {121.0, 145.0, 100.0}},
                                                  {200, {150.0, 180.0, 130.0}}};
    REQUIRE(scm::estimateCapacityBreakpoint(bins).has_value());
    REQUIRE_NEAR(*scm::estimateCapacityBreakpoint(bins), 150.0, 0.001);
}

TEST_CASE("capacity breakpoint reports insufficient data") {
    const std::vector<scm::CapacityBinInput> sparse{{0, {100.0, std::nullopt, std::nullopt}},
                                                    {100, {200.0, std::nullopt, std::nullopt}}};
    REQUIRE(!scm::estimateCapacityBreakpoint(sparse).has_value());
}

TEST_CASE("mechanical load bin boundaries match the configured MVP ranges") {
    REQUIRE(scm::mechanicalLoadBin(0.0) == 0);
    REQUIRE(scm::mechanicalLoadBin(99.9) == 0);
    REQUIRE(scm::mechanicalLoadBin(100.0) == 1);
    REQUIRE(scm::mechanicalLoadBin(149.9) == 1);
    REQUIRE(scm::mechanicalLoadBin(150.0) == 2);
    REQUIRE(scm::mechanicalLoadBin(200.0) == 3);
    REQUIRE(scm::mechanicalLoadBin(250.0) == 4);
    REQUIRE(scm::mechanicalLoadBin(300.0) == 5);
}
