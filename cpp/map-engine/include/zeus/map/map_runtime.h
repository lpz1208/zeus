#pragma once

#include <memory>
#include <span>
#include <vector>

#include "zeus/map/types.h"

namespace zeus::map {

class MapRuntime {
public:
    explicit MapRuntime(MapData map);
    ~MapRuntime();

    MapRuntime(MapRuntime&&) noexcept;
    MapRuntime& operator=(MapRuntime&&) noexcept;
    MapRuntime(const MapRuntime&) = delete;
    MapRuntime& operator=(const MapRuntime&) = delete;

    [[nodiscard]] const MapData& data() const;
    [[nodiscard]] const DirectedEdge& edge(EdgeIndex edge) const;
    [[nodiscard]] std::span<const EdgeIndex> outgoingEdges(NodeIndex node) const;
    [[nodiscard]] bool hasTurnTransitions() const;
    // Returns +infinity for a prohibited transition, otherwise its extra
    // generalized travel-time cost in seconds.
    [[nodiscard]] double turnPenaltySeconds(EdgeIndex from_edge, EdgeIndex to_edge) const;

    [[nodiscard]] std::vector<MapMatchCandidate> matchPoint(
        Point2d point,
        const MapMatchOptions& options = {}) const;

    [[nodiscard]] WorldPose worldPose(const VehicleMapPosition& position) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zeus::map
