#include "test_framework.h"

#include "analysis/statistics.h"

#include <vector>

TEST_CASE("statistics use deterministic R-7 percentiles") {
    const std::vector<double> values{1, 2, 3, 4, 5};
    REQUIRE_NEAR(*scm::median(values), 3.0, 0.0001);
    REQUIRE_NEAR(*scm::percentile(values, 0.75), 4.0, 0.0001);
    REQUIRE_NEAR(*scm::percentile(values, 0.90), 4.6, 0.0001);
    REQUIRE_NEAR(*scm::mad(values), 1.0, 0.0001);
    REQUIRE_NEAR(*scm::mean(values), 3.0, 0.0001);
}

TEST_CASE("statistics suppress undersized samples") {
    REQUIRE(!scm::median({1, 2, 3, 4}).has_value());
    REQUIRE(!scm::percentageChange(0.0, 10.0).has_value());
    REQUIRE_NEAR(*scm::percentageChange(100.0, 80.0), -20.0, 0.0001);
}

TEST_CASE("rectangle IoU handles overlap and empty rectangles") {
    REQUIRE_NEAR(scm::rectangleIou(0, 0, 10, 10, 5, 5, 15, 15), 25.0 / 175.0, 0.0001);
    REQUIRE_NEAR(scm::rectangleIou(0, 0, 0, 0, 0, 0, 0, 0), 0.0, 0.0001);
}
