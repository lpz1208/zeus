#pragma once

#include <set>
#include "zeus/routing/search.h"

namespace zeus::routing {
// Backward LPA* (h=0) or D* Lite on directed incoming-edge states. A virtual source
// connects the current partial-edge origin options. Changing those connections
// supports vehicle movement without discarding the goal-distance labels.
// Owned by one caller; never shared between concurrent sessions/engine threads.
class IncrementalSearch {
public:
    explicit IncrementalSearch(const zeus::map::MapRuntime& runtime);
    [[nodiscard]] SearchOutput run(const SearchQuery& query);
    [[nodiscard]] const zeus::map::MapRuntime& runtime() const { return runtime_; }
private:
    using State = zeus::map::EdgeIndex;
    using Key = std::pair<double, double>;
    const zeus::map::MapRuntime& runtime_;
    IncomingAdjacency incoming_;
    std::vector<double> g_, rhs_, factors_;
    std::vector<std::uint8_t> enabled_;
    std::set<std::pair<Key, State>> queue_;
    std::vector<Key> queued_keys_;
    SearchQuery query_;
    bool initialized_ = false;
    State source_;
    double heuristic_scale_ = 0, radius_ = 0, km_ = 0;
    zeus::map::Point2d anchor_;
    Key key(State state) const;
    void enqueue(State state);
    double transition(State from, State to) const;
    double terminal(State state, std::size_t goal) const;
    void update(State state);
    void updatePredecessors(State state);
};
}  // namespace zeus::routing
