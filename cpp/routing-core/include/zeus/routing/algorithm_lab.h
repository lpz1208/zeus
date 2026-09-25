#pragma once
#include <ostream>
#include <string_view>
#include "zeus/routing/route_planner.h"

namespace zeus::routing {
// Edge-state graph. -1 is the virtual origin; -2 is the virtual destination.
// Keeping the incoming edge in the state makes turn restrictions unavoidable.
struct LabTransition {
    int state;
    zeus::map::EdgeIndex edge;
    double cost;
    double length;
};
class AlgorithmLab {
public:
    AlgorithmLab(const zeus::map::MapRuntime& runtime, RouteRequest request);
    const RouteResult& baseline() const { return baseline_; }
    std::vector<LabTransition> neighbors(int state) const;
    RouteResult validate(const std::vector<int>& states) const;
    RouteResult validatePath(const RoutePath& path) const;
    void writeContext(std::ostream& output, std::string_view observation = "{\"mode\":\"static\"}") const;
private:
    const zeus::map::MapRuntime& runtime_;
    RouteRequest request_;
    RouteResult baseline_;
    std::vector<RoutePosition> starts_, goals_;
    bool enabled(zeus::map::EdgeIndex edge) const;
    double seconds(zeus::map::EdgeIndex edge, double length) const;
};
}
