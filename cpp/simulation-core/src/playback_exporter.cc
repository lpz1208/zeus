#include "zeus/simulation/playback_exporter.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace zeus::simulation {
namespace {

struct DatasetCloser {
    void operator()(GDALDataset* dataset) const {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

struct TransformDeleter {
    void operator()(OGRCoordinateTransformation* transform) const {
        OGRCoordinateTransformation::DestroyCT(transform);
    }
};

using TransformPtr = std::unique_ptr<OGRCoordinateTransformation, TransformDeleter>;

TransformPtr createRuntimeToWgs84(const zeus::map::MapRuntime& runtime) {
    const std::string& wkt = runtime.data().metadata.runtime_crs_wkt;
    if (wkt.empty()) {
        throw std::runtime_error("cannot export a simulation without a runtime CRS");
    }
    OGRSpatialReference source;
    if (source.importFromWkt(wkt.c_str()) != OGRERR_NONE) {
        throw std::runtime_error("runtime map contains an invalid CRS");
    }
    source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    TransformPtr transform(OGRCreateCoordinateTransformation(&source, &wgs84));
    if (!transform) {
        throw std::runtime_error("cannot create runtime-to-WGS84 transformation");
    }
    return transform;
}

void createField(OGRLayer& layer, const char* name, OGRFieldType type) {
    OGRFieldDefn field(name, type);
    if (layer.CreateField(&field) != OGRERR_NONE) {
        throw std::runtime_error(std::string("cannot create trajectory field: ") + name);
    }
}

zeus::map::Point2d samplePoint(
    const zeus::map::MapRuntime& runtime,
    const VehicleSample& sample) {
    const zeus::map::WorldPose pose =
        runtime.worldPose({sample.edge, sample.offset_s, 0, 0.0F});
    return pose.point;
}

}  // namespace

std::size_t TrajectoryExporter::save(
    const zeus::map::MapRuntime& runtime,
    const SimulationResult& result,
    const std::string& path) {
    if (!result.ok) {
        throw std::runtime_error("cannot export a failed simulation");
    }
    TransformPtr transform = createRuntimeToWgs84(runtime);

    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GeoJSON");
    if (driver == nullptr) {
        throw std::runtime_error("GDAL GeoJSON driver is unavailable");
    }
    if (std::filesystem::exists(path)) {
        std::filesystem::remove(path);
    }
    std::unique_ptr<GDALDataset, DatasetCloser> dataset(
        driver->Create(path.c_str(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!dataset) {
        throw std::runtime_error("cannot create trajectory GeoJSON output: " + path);
    }
    OGRSpatialReference wgs84;
    wgs84.SetWellKnownGeogCS("WGS84");
    wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRLayer* layer = dataset->CreateLayer("trajectories", &wgs84, wkbLineString, nullptr);
    if (layer == nullptr) {
        throw std::runtime_error("cannot create trajectory GeoJSON layer");
    }
    createField(*layer, "VEHICLE_ID", OFTInteger);
    createField(*layer, "DEPART_S", OFTReal);
    createField(*layer, "ARRIVE_S", OFTReal);
    createField(*layer, "TRAVEL_S", OFTReal);
    createField(*layer, "DISTANCE_M", OFTReal);

    std::size_t written = 0;
    for (const VehicleRecord& record : result.vehicles) {
        if (record.samples.size() < 2 || record.route_id >= result.routes.size()) {
            continue;
        }
        OGRLineString line;
        for (const VehicleSample& sample : record.samples) {
            const zeus::map::Point2d point = samplePoint(runtime, sample);
            line.addPoint(point.x, point.y);
        }
        if (line.transform(transform.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot transform trajectory to WGS84");
        }

        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(layer->GetLayerDefn()), &OGRFeature::DestroyFeature);
        feature->SetField("VEHICLE_ID", static_cast<int>(record.id));
        if (std::isfinite(record.actual_depart_s)) {
            feature->SetField("DEPART_S", record.actual_depart_s);
        }
        if (std::isfinite(record.arrive_s)) {
            feature->SetField("ARRIVE_S", record.arrive_s);
        }
        if (std::isfinite(record.arrive_s) && std::isfinite(record.actual_depart_s)) {
            feature->SetField("TRAVEL_S", record.arrive_s - record.actual_depart_s);
        }
        feature->SetField("DISTANCE_M", record.traveled_m);
        feature->SetGeometry(&line);
        if (layer->CreateFeature(feature.get()) != OGRERR_NONE) {
            throw std::runtime_error("cannot write a trajectory feature to GeoJSON");
        }
        ++written;
    }
    return written;
}

void PlaybackExporter::save(
    const zeus::map::MapRuntime& runtime,
    const SimulationResult& result,
    const std::string& path) {
    if (!result.ok) {
        throw std::runtime_error("cannot export a failed simulation");
    }
    TransformPtr transform = createRuntimeToWgs84(runtime);

    std::vector<double> xs;
    std::vector<double> ys;
    std::size_t total_samples = 0;
    for (const VehicleRecord& record : result.vehicles) {
        total_samples += record.samples.size();
    }
    xs.reserve(total_samples);
    ys.reserve(total_samples);
    for (const VehicleRecord& record : result.vehicles) {
        for (const VehicleSample& sample : record.samples) {
            const zeus::map::Point2d point = samplePoint(runtime, sample);
            xs.push_back(point.x);
            ys.push_back(point.y);
        }
    }
    if (!xs.empty() &&
        !transform->Transform(static_cast<int>(xs.size()), xs.data(), ys.data())) {
        throw std::runtime_error("cannot transform playback samples to WGS84");
    }

    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("cannot create playback output: " + path);
    }
    output << std::fixed << std::setprecision(6);
    output << "{\n"
           << "  \"duration_s\": " << std::setprecision(3) << result.config.duration_seconds
           << ",\n"
           << "  \"step_s\": " << result.config.step_seconds << ",\n"
           << "  \"sample_interval_s\": " << result.config.sample_interval_seconds << ",\n"
           << "  \"reroute_interval_s\": "
           << result.config.reroute_interval_seconds << ",\n"
           << "  \"reroute_cost_ratio\": " << result.config.reroute_cost_ratio << ",\n"
           << "  \"cancelled\": "
           << (result.stats.cancelled ? "true" : "false") << ",\n"
           << "  \"barrier_wait_ms\": " << result.stats.barrier_wait_ms << ",\n"
           << "  \"compute_ms\": " << result.stats.compute_ms << ",\n"
           << "  \"controls\": [";
    for (std::size_t i = 0; i < result.applied_controls.size(); ++i) {
        const AppliedControlEvent& control = result.applied_controls[i];
        if (i > 0) {
            output << ", ";
        }
        output << "{\"requested_s\": " << std::setprecision(3)
               << control.requested_time_s
               << ", \"effective_s\": " << control.effective_time_s
               << ", \"scope\": \"" << controlScopeName(control.scope)
               << "\", \"target_id\": " << control.target_id
               << ", \"action\": \"" << controlActionName(control.action)
               << "\", \"value\": " << control.value << '}';
    }
    output << "],\n"
           << "  \"reroutes\": [";
    for (std::size_t i = 0; i < result.reroutes.size(); ++i) {
        const VehicleRerouteRecord& reroute = result.reroutes[i];
        if (i > 0) {
            output << ", ";
        }
        output << "{\"time_s\": " << std::setprecision(3) << reroute.time_s
               << ", \"vehicle_id\": " << reroute.vehicle_id
               << ", \"old_route_id\": " << reroute.old_route_id
               << ", \"new_route_id\": " << reroute.new_route_id
               << ", \"success\": " << (reroute.success ? "true" : "false")
               << '}';
    }
    output << "],\n"
           << "  \"edge_kpis\": [";
    for (std::size_t i = 0; i < result.edge_kpis.size(); ++i) {
        const EdgeKpi& kpi = result.edge_kpis[i];
        if (i > 0) {
            output << ", ";
        }
        const auto writeKpiValue = [&output](double value) {
            if (std::isfinite(value)) {
                output << std::setprecision(3) << value;
            } else {
                output << '0';
            }
        };
        output << "{\"edge_id\": " << kpi.edge
               << ", \"entries\": " << kpi.entries
               << ", \"vehicle_seconds_s\": ";
        writeKpiValue(kpi.vehicle_seconds);
        output << ", \"distance_m\": ";
        writeKpiValue(kpi.distance_m);
        output << ", \"mean_speed_mps\": ";
        writeKpiValue(kpi.mean_speed_mps);
        output << '}';
    }
    output << "],\n"
           << "  \"signal_plans\": [";
    for (std::size_t plan_index = 0; plan_index < result.signal_plans.size(); ++plan_index) {
        const JunctionSignalPlan& plan = result.signal_plans[plan_index];
        if (plan_index > 0) {
            output << ", ";
        }
        output << "{\"node_id\": " << plan.node
               << ", \"offset_s\": " << std::setprecision(3) << plan.offset_seconds
               << ", \"yellow_s\": " << plan.yellow_seconds
               << ", \"all_red_s\": " << plan.all_red_seconds
               << ", \"phases\": [";
        for (std::size_t phase_index = 0; phase_index < plan.phases.size(); ++phase_index) {
            const SignalPhase& phase = plan.phases[phase_index];
            if (phase_index > 0) {
                output << ", ";
            }
            output << "{\"green_s\": " << phase.green_seconds
                   << ", \"saturation_flow_vph\": "
                   << phase.saturation_flow_vph
                   << ", \"movements\": [";
            for (std::size_t movement_index = 0;
                 movement_index < phase.movements.size(); ++movement_index) {
                const SignalMovement& movement = phase.movements[movement_index];
                if (movement_index > 0) {
                    output << ", ";
                }
                output << "[" << movement.from_edge << ", " << movement.to_edge << "]";
            }
            output << "]}";
        }
        output << "]}";
    }
    output << "],\n"
           << "  \"vehicles\": [\n";
    std::size_t written_vehicles = 0;
    std::size_t vehicles_with_samples = 0;
    for (const VehicleRecord& record : result.vehicles) {
        if (!record.samples.empty()) {
            ++vehicles_with_samples;
        }
    }
    std::size_t sample_cursor = 0;
    for (const VehicleRecord& record : result.vehicles) {
        if (record.samples.empty()) {
            continue;
        }
        const auto writeSeconds = [&output](double seconds) {
            if (std::isfinite(seconds)) {
                output << std::setprecision(3) << seconds;
            } else {
                output << "null";
            }
        };
        output << "    {\"id\": " << record.id << ", \"depart_s\": ";
        writeSeconds(record.actual_depart_s);
        output << ", \"arrive_s\": ";
        writeSeconds(record.arrive_s);
        output << ", \"samples\": [";
        for (std::size_t i = 0; i < record.samples.size(); ++i) {
            if (i > 0) {
                output << ", ";
            }
            output << '[' << std::setprecision(3) << record.samples[i].t
                   << ", " << std::setprecision(6) << xs[sample_cursor + i]
                   << ", " << ys[sample_cursor + i] << ']';
        }
        output << "]}";
        if (++written_vehicles < vehicles_with_samples) {
            output << ',';
        }
        output << '\n';
        sample_cursor += record.samples.size();
    }
    output << "  ]\n}\n";
}

}  // namespace zeus::simulation
