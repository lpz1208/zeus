#pragma once

#include <unordered_map>
#include "zeus/routing/search.h"

namespace zeus::routing {
struct SpeedChange {
    zeus::map::EdgeIndex edge;
    double time_s;
    double factor;
};

// Positive, piecewise-constant speeds integrated across the entire traversal.
// This yields FIFO arrival functions, including partial edges. Immutable after
// construction; equal-time changes apply in input order (last one wins).
class SpeedSchedule {
public:
    SpeedSchedule() = default;
    SpeedSchedule(std::size_t edge_count, std::span<const SpeedChange> changes);
    [[nodiscard]] double duration(const zeus::map::MapRuntime& runtime,
        zeus::map::EdgeIndex edge, double length_m, double departure_s,
        double overlay_factor = 1, double reference_s = 0) const;
private:
    std::unordered_map<zeus::map::EdgeIndex, std::vector<SpeedChange>> profiles_;
};

[[nodiscard]] SearchOutput runTimeDependentSearch(const zeus::map::MapRuntime& runtime,
    const SearchQuery& query, const RouteRequest& request, const SpeedSchedule& schedule,
    double known_best_time_s);
}  // namespace zeus::routing
