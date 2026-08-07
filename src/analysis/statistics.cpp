#include "analysis/statistics.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace scm {

std::optional<double> percentile(std::vector<double> values, double probability, std::size_t minimumCount) {
    if (values.size() < minimumCount || values.empty() || probability < 0.0 || probability > 1.0)
        return std::nullopt;
    std::sort(values.begin(), values.end());
    const double position = (static_cast<double>(values.size()) - 1.0) * probability;
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper)
        return values[lower];
    const double weight = position - static_cast<double>(lower);
    return values[lower] * (1.0 - weight) + values[upper] * weight;
}

std::optional<double> median(std::vector<double> values, std::size_t minimumCount) {
    return percentile(std::move(values), 0.5, minimumCount);
}

std::optional<double> mean(const std::vector<double>& values, std::size_t minimumCount) {
    if (values.size() < minimumCount || values.empty())
        return std::nullopt;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

std::optional<double> mad(const std::vector<double>& values, std::size_t minimumCount) {
    const auto center = median(values, minimumCount);
    if (!center)
        return std::nullopt;
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const auto value : values)
        deviations.push_back(std::abs(value - *center));
    return median(std::move(deviations), minimumCount);
}

std::optional<double> ratio(double numerator, double denominator) {
    if (denominator == 0.0)
        return std::nullopt;
    return numerator / denominator;
}

std::optional<double> percentageChange(double baseline, double current) {
    if (baseline == 0.0)
        return std::nullopt;
    return (current - baseline) / baseline * 100.0;
}

Distribution describe(const std::vector<double>& values, std::size_t minimumCount) {
    Distribution result;
    result.count = values.size();
    result.median = percentile(values, 0.5, minimumCount);
    result.p75 = percentile(values, 0.75, minimumCount);
    result.p90 = percentile(values, 0.90, minimumCount);
    result.p95 = percentile(values, 0.95, minimumCount);
    result.mad = scm::mad(values, minimumCount);
    result.mean = scm::mean(values, minimumCount);
    if (values.size() >= minimumCount && !values.empty())
        result.maximum = *std::max_element(values.begin(), values.end());
    return result;
}

double rectangleIou(double leftA, double topA, double rightA, double bottomA, double leftB, double topB, double rightB,
                    double bottomB) {
    const double intersectionWidth = std::max(0.0, std::min(rightA, rightB) - std::max(leftA, leftB));
    const double intersectionHeight = std::max(0.0, std::min(bottomA, bottomB) - std::max(topA, topB));
    const double intersection = intersectionWidth * intersectionHeight;
    const double areaA = std::max(0.0, rightA - leftA) * std::max(0.0, bottomA - topA);
    const double areaB = std::max(0.0, rightB - leftB) * std::max(0.0, bottomB - topB);
    const double unionArea = areaA + areaB - intersection;
    return unionArea <= 0.0 ? 0.0 : std::clamp(intersection / unionArea, 0.0, 1.0);
}

} // namespace scm
