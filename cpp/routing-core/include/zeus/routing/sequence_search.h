#pragma once
#include <stdexcept>
#include "zeus/routing/search.h"
#include "zeus/routing/time_dependent.h"

namespace zeus::routing {
// Prefix states uniquely determine their last road. Only root requires a
// separate road ID, so the product graph needs E + prefix_count - 1 states.
class SequenceGraph {
public:
    explicit SequenceGraph(const zeus::map::MapRuntime& runtime) : runtime_(runtime), edges_(runtime.data().edges.size()) {}
    std::size_t size() const { return edges_ + runtime_.turnStateCount() - 1; }
    std::uint32_t encode(zeus::map::EdgeIndex edge, std::uint32_t context) const {
        return context ? static_cast<std::uint32_t>(edges_ + context - 1) : edge;
    }
    zeus::map::EdgeIndex edge(std::uint32_t state) const {
        return state < edges_ ? state : runtime_.turnStateEdge(state - edges_ + 1);
    }
    std::uint32_t context(std::uint32_t state) const { return state < edges_ ? 0 : state - edges_ + 1; }
    std::uint32_t initial(const RouteRequest& request, zeus::map::EdgeIndex edge) const {
        if (!request.origin_turn_state) return runtime_.advanceTurnState(0, edge);
        const auto state = *request.origin_turn_state;
        if (!request.origin_position || request.origin_position->edge != edge ||
            state >= runtime_.turnStateCount() ||
            (state && runtime_.turnStateEdge(state) != edge) ||
            (!state && runtime_.advanceTurnState(0, edge) != 0))
            throw std::invalid_argument("turn history does not match exact origin");
        return state;
    }
private:
    const zeus::map::MapRuntime& runtime_;
    std::size_t edges_;
};
SearchOutput runSequenceSearch(const zeus::map::MapRuntime& runtime, const SearchQuery& query,
    const RouteRequest& request, const SpeedSchedule& schedule, double known_best_time_s);
}  // namespace zeus::routing
