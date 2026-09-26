#pragma once

#include "zeus/routing/search.h"

namespace zeus::routing {
// Immutable directed landmark distances on the relaxed base-cost node graph.
// Restrictions and overlay penalties are enforced by the actual edge-state search.
class LandmarkIndex {
public:
    static constexpr std::size_t kDefaultCount = 8;
    static constexpr std::size_t kTableBudget = 256 * 1024 * 1024;
    LandmarkIndex(const zeus::map::MapRuntime& runtime, const IncomingAdjacency& incoming,
        std::size_t requested = kDefaultCount, std::size_t budget = kTableBudget);
    [[nodiscard]] double lowerBound(zeus::map::NodeIndex from, zeus::map::NodeIndex to) const;
    [[nodiscard]] std::size_t count() const { return landmarks_.size(); }
    [[nodiscard]] std::size_t tableBytes() const { return count() * node_count_ * 2 * sizeof(double); }
    [[nodiscard]] const zeus::map::MapRuntime& runtime() const { return runtime_; }
private:
    const zeus::map::MapRuntime& runtime_;
    std::size_t node_count_;
    std::vector<zeus::map::NodeIndex> landmarks_;
    std::vector<std::vector<double>> from_landmark_, to_landmark_;
};

[[nodiscard]] SearchOutput runAltSearch(const zeus::map::MapRuntime& runtime,
    const SearchQuery& query, const LandmarkIndex& landmarks, double known_best_time_s);
}  // namespace zeus::routing
