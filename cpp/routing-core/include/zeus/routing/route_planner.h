#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "zeus/map/map_runtime.h"
#include "zeus/map/types.h"

#include "zeus/routing/route_types.h"
#include "zeus/routing/search.h"

namespace zeus::routing {
class IncrementalSearch;
class LandmarkIndex;
class ContractionHierarchy;

// Plans routes over a read-only MapRuntime: snaps the endpoints to edges,
// expands forward/reverse traversal options via twin edges, runs the shortest
// path search, and assembles the final partial-edge path.
class RoutePlanner {
public:
    explicit RoutePlanner(const zeus::map::MapRuntime& runtime);

    [[nodiscard]] RouteResult plan(const RouteRequest& request, IncrementalSearch* incremental = nullptr) const;
    [[nodiscard]] double maxSpeedMps() const { return max_speed_mps_; }

private:
    [[nodiscard]] zeus::map::EdgeIndex findTwin(zeus::map::EdgeIndex edge) const;

    const zeus::map::MapRuntime& runtime_;
    double max_speed_mps_ = kMinSpeedMps;
    std::unordered_map<std::uint64_t, std::vector<zeus::map::EdgeIndex>> edges_by_pair_;
    IncomingAdjacency incoming_;
    mutable std::once_flag ch_once_;
    mutable std::shared_ptr<const ContractionHierarchy> hierarchy_;
    mutable std::once_flag landmark_once_;
    mutable std::shared_ptr<const LandmarkIndex> landmarks_;
};

}  // namespace zeus::routing
