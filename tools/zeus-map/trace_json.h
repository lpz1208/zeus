#pragma once

#include <cstddef>
#include <ostream>
#include <vector>

#include "zeus/routing/route_types.h"

namespace zeus {

// Writes a search settle trace as the JSON object consumed by the web
// workbenches: {"stepCount":N,"sampled":b,"steps":[{"order","nodeId","f","g"}]}.
// stepCount carries the true settle total (orders survive stride sampling) so
// the frontend can label progress even when steps were compacted.
inline void writeSearchTrace(
    std::ostream& out, const std::vector<routing::SearchTraceStep>& trace) {
    const std::uint32_t step_count = trace.empty() ? 0 : trace.back().order;
    out << "{\"stepCount\": " << step_count
        << ", \"sampled\": " << (trace.size() < step_count ? "true" : "false")
        << ", \"steps\": [";
    for (std::size_t i = 0; i < trace.size(); ++i) {
        const routing::SearchTraceStep& step = trace[i];
        if (i > 0) {
            out << ", ";
        }
        out << "{\"order\": " << step.order
            << ", \"nodeId\": " << step.node
            << ", \"f\": " << step.f
            << ", \"g\": " << step.g << "}";
    }
    out << "]}";
}

}  // namespace zeus
