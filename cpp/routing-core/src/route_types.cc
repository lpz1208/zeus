#include "zeus/routing/route_types.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace zeus::routing {

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
