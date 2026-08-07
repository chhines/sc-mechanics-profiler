#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace scm {

struct Distribution {
    std::size_t count{};
    std::optional<double> median;
    std::optional<double> p75;
    std::optional<double> p90;
    std::optional<double> p95;
    std::optional<double> mad;
    std::optional<double> mean;
    std::optional<double> maximum;
};

// Uses R-7 linear interpolation: h=(n-1)p, interpolate between floor(h) and ceil(h).
std::optional<double> percentile(std::vector<double> values, double probability, std::size_t minimumCount = 5);
std::optional<double> median(std::vector<double> values, std::size_t minimumCount = 5);
std::optional<double> mean(const std::vector<double>& values, std::size_t minimumCount = 5);
std::optional<double> mad(const std::vector<double>& values, std::size_t minimumCount = 5);
std::optional<double> ratio(double numerator, double denominator);
std::optional<double> percentageChange(double baseline, double current);
Distribution describe(const std::vector<double>& values, std::size_t minimumCount = 5);
double rectangleIou(double leftA, double topA, double rightA, double bottomA, double leftB, double topB, double rightB,
                    double bottomB);

} // namespace scm
