#pragma once

#include "zeus/routing/search.h"

namespace zeus::routing {

// Static directed CH: node states without turns, incoming-edge states with
// turn rules. Partial contraction leaves an exact searchable core.
// Immutable after construction; query labels are local.
class ContractionHierarchy {
public:
    struct Limits {
        std::size_t max_arcs = 4000000;
        std::uint64_t max_work = 50000000;
        std::size_t witness_settles = 128;
        std::size_t max_states = 1000000;
    };
    explicit ContractionHierarchy(const zeus::map::MapRuntime& runtime);
    ContractionHierarchy(const zeus::map::MapRuntime& runtime, Limits limits);
    [[nodiscard]] SearchOutput search(const SearchQuery& query, const IncomingAdjacency& incoming,
                                      double known_best_time_s) const;
    [[nodiscard]] bool available() const { return available_; }
    [[nodiscard]] std::size_t shortcuts() const { return shortcuts_; }
    [[nodiscard]] std::size_t coreStates() const { return core_states_; }
    [[nodiscard]] std::size_t bytes() const;
    [[nodiscard]] const zeus::map::MapRuntime& runtime() const { return runtime_; }
    [[nodiscard]] static bool baseWeights(const RoutingOverlay* overlay);
private:
    struct Arc {
        zeus::map::EdgeIndex from, to;
        double cost;
        std::uint32_t left, right; // Shortcut children; base arc: left=invalid, right=road ID.
    };
    const zeus::map::MapRuntime& runtime_;
    std::vector<Arc> arcs_;
    std::vector<std::vector<std::uint32_t>> out_, in_;
    std::vector<std::uint32_t> rank_;
    std::size_t shortcuts_ = 0, core_states_ = 0;
    bool turn_aware_ = false;
    bool available_ = false;
};
}  // namespace zeus::routing
