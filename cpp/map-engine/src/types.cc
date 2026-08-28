#include "zeus/map/types.h"

#include <cmath>

namespace zeus::map {

double distance(Point2d lhs, Point2d rhs) {
    return std::hypot(rhs.x - lhs.x, rhs.y - lhs.y);
}

double polylineLength(const std::vector<Point2d>& points) {
    double result = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        result += distance(points[i - 1], points[i]);
    }
    return result;
}

PersistentId stableId(const std::string& value) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t result = kOffset;
    for (const unsigned char byte : value) {
        result ^= byte;
        result *= kPrime;
    }
    return result;
}

bool ValidationReport::hasFatalErrors() const {
    return count(IssueSeverity::kFatal) > 0;
}

std::size_t ValidationReport::count(IssueSeverity severity) const {
    std::size_t result = 0;
    for (const ValidationIssue& issue : issues) {
        if (issue.severity == severity) {
            ++result;
        }
    }
    return result;
}

}  // namespace zeus::map

