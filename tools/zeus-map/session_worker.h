#pragma once

#include "zeus/map/map_runtime.h"

namespace zeus::cli {

// Resident session worker: reads tab-delimited commands from stdin and writes
// ZEUS_SESSION_RESPONSE frames to stdout, mirroring the route-worker framing.
// The map, routing planner and engine are constructed once; sessions live in
// a registry keyed by caller-supplied session ids.
int runSessionWorker(const zeus::map::MapRuntime& runtime);

}  // namespace zeus::cli
