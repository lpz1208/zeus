#include "zeus/routing/route_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace zeus::routing {
namespace {

constexpr std::array<AlgorithmCapability, 4> kCapabilities = {{
    {Algorithm::kDijkstra, "1", "forward", true, false, false, false,
     true, true, false},
    {Algorithm::kAStar, "1", "forward", true, false, false, false,
     true, true, true},
    {Algorithm::kBidirectionalDijkstra, "1", "bidirectional", true, false,
     false, false, true, true, false},
    {Algorithm::kBidirectionalAStar, "1", "bidirectional", true, false,
     false, false, true, true, true},
}};

}  // namespace

const char* algorithmName(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::kAStar:
            return "astar";
        case Algorithm::kBidirectionalDijkstra:
            return "bidijkstra";
        case Algorithm::kBidirectionalAStar:
            return "biastar";
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
    return false;
}

bool isBidirectional(Algorithm algorithm) {
    return algorithm == Algorithm::kBidirectionalDijkstra ||
           algorithm == Algorithm::kBidirectionalAStar;
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
