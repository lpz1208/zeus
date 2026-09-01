#pragma once

#include <string>
#include <vector>

#include "zeus/routing/route_types.h"
#include "zeus/simulation/simulation_types.h"

namespace zeus::cli {

[[nodiscard]] std::string trim(const std::string& value);

[[nodiscard]] std::vector<std::string> splitTabs(const std::string& line);

// Transforms WGS84 lon/lat arrays in place into the runtime CRS.
void transformWgs84Batch(
    std::vector<double>& xs,
    std::vector<double>& ys,
    const std::string& runtime_crs_wkt);

// CSV rows: time_s,vehicle|edge|junction,target_id,action[,value]
[[nodiscard]] std::vector<zeus::simulation::SimulationControlEvent> parseSimulationControls(
    const std::string& path);

// CSV rows: node_id,phase,green_s,yellow_s,all_red_s,offset_s,from_edge,to_edge[,
// saturation_flow_vph]
[[nodiscard]] std::vector<zeus::simulation::JunctionSignalPlan> parseSignalPlans(
    const std::string& path);

// One WGS84 origin-destination row from an od file:
// lon,lat,dest_lon,dest_lat,depart_s[,algorithm][,agent]
struct OdRow {
    double origin_lon = 0.0;
    double origin_lat = 0.0;
    double dest_lon = 0.0;
    double dest_lat = 0.0;
    double depart_s = 0.0;
    std::string algorithm;
    bool agent_controlled = false;
};

[[nodiscard]] std::vector<OdRow> parseOdFile(const std::string& path);

// Batch-transforms the rows into runtime CRS VehicleDemands. Per-row
// algorithms override the default; unknown names throw.
[[nodiscard]] std::vector<zeus::simulation::VehicleDemand> buildVehicleDemands(
    const std::vector<OdRow>& rows,
    zeus::routing::Algorithm default_algorithm,
    const std::string& runtime_crs_wkt);

}  // namespace zeus::cli
