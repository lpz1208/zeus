#include "zeus/routing/route_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace zeus::routing {
namespace {

constexpr std::array<AlgorithmCapability, 10> kCapabilities = {{
    {Algorithm::kDijkstra, "1", "forward", true, false, false, false,
     true, true, false},
    {Algorithm::kAStar, "1", "forward", true, false, false, false,
     true, true, true},
    {Algorithm::kBidirectionalDijkstra, "2", "bidirectional", true, false,
     false, false, true, true, false},
    {Algorithm::kBidirectionalAStar, "2", "bidirectional", true, false,
     false, false, true, true, true},
    {Algorithm::kCH, "1", "bidirectional", false, false, false, false,
     true, true, false},
    {Algorithm::kAlt, "1", "forward", true, false, false, false,
     true, true, true},
    {Algorithm::kTimeDependent, "1", "forward", true, false, false, true,
     true, true, false},
    {Algorithm::kLpaStar, "1", "backward", true, true, false, false,
     true, true, false},
    {Algorithm::kDStarLite, "1", "backward", true, true, false, false,
     true, true, true},
    {Algorithm::kKShortest, "1", "forward", true, false, true, false,
     true, true, false},
}};

}  // namespace

const char* algorithmName(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::kCH:
            return "ch";
        case Algorithm::kAlt:
            return "alt";
        case Algorithm::kTimeDependent:
            return "tddijkstra";
        case Algorithm::kDStarLite:
            return "dstar";
        case Algorithm::kLpaStar:
            return "lpa";
        case Algorithm::kAStar:
            return "astar";
        case Algorithm::kBidirectionalDijkstra:
            return "bidijkstra";
        case Algorithm::kBidirectionalAStar:
            return "biastar";
        case Algorithm::kKShortest:
            return "kshortest";
        case Algorithm::kDijkstra:
            break;
    }
    return "dijkstra";
}

bool parseAlgorithm(const std::string& value, Algorithm& algorithm) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        normalized.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(character))));
    }
    if (normalized == "ch") {
        algorithm = Algorithm::kCH;
        return true;
    }
    if (normalized == "alt") {
        algorithm = Algorithm::kAlt;
        return true;
    }
    if (normalized == "tddijkstra") {
        algorithm = Algorithm::kTimeDependent;
        return true;
    }
    if (normalized == "dstar" || normalized == "dstar-lite" || normalized == "d*lite") {
        algorithm = Algorithm::kDStarLite;
        return true;
    }
    if (normalized == "lpa" || normalized == "lpa*") {
        algorithm = Algorithm::kLpaStar;
        return true;
    }
    if (normalized == "dijkstra") {
        algorithm = Algorithm::kDijkstra;
        return true;
    }
    if (normalized == "astar" || normalized == "a*") {
        algorithm = Algorithm::kAStar;
        return true;
    }
    if (normalized == "bidijkstra" || normalized == "bidirectional-dijkstra") {
        algorithm = Algorithm::kBidirectionalDijkstra;
        return true;
    }
    if (normalized == "biastar" || normalized == "bidirectional-astar") {
        algorithm = Algorithm::kBidirectionalAStar;
        return true;
    }
    if (normalized == "kshortest" || normalized == "k-shortest" ||
        normalized == "kshortest-paths" || normalized == "yen") {
        algorithm = Algorithm::kKShortest;
        return true;
    }
    return false;
}

bool isIncremental(Algorithm algorithm) {
    return algorithm == Algorithm::kLpaStar || algorithm == Algorithm::kDStarLite;
}

bool isBidirectional(Algorithm algorithm) {
    return algorithm == Algorithm::kBidirectionalDijkstra ||
           algorithm == Algorithm::kBidirectionalAStar || algorithm == Algorithm::kCH;
}

std::span<const AlgorithmCapability> algorithmCapabilities() {
    return kCapabilities;
}

const AlgorithmCapability* algorithmCapability(Algorithm algorithm) {
    const auto found = std::find_if(
        kCapabilities.begin(), kCapabilities.end(),
        [algorithm](const AlgorithmCapability& capability) {
            return capability.algorithm == algorithm;
        });
    return found == kCapabilities.end() ? nullptr : &*found;
}

const char* routeFailureName(RouteFailure failure) {
    switch (failure) {
        case RouteFailure::kEmptyMap:
            return "empty_map";
        case RouteFailure::kOriginUnmatched:
            return "unmatched_origin";
        case RouteFailure::kDestinationUnmatched:
            return "unmatched_destination";
        case RouteFailure::kUnreachable:
            return "unreachable";
        case RouteFailure::kNone:
            break;
    }
    return "none";
}

}  // namespace zeus::routing
