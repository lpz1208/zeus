#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace zeus::map {

struct OsmPreprocessOptions {
    bool include_service = false;
    bool include_track = false;
    bool include_private = false;
    double min_length_m = 2.0;
};

struct OsmPreprocessReport {
    std::uint64_t input_features = 0;
    std::uint64_t output_features = 0;
    std::uint64_t filtered_features = 0;
    std::uint64_t geometry_collections_converted = 0;
    std::uint64_t default_speed_applied = 0;
    std::uint64_t mph_speed_converted = 0;
    std::uint64_t implied_oneway_applied = 0;
    std::uint64_t reverse_oneway_normalized = 0;
    std::uint64_t duplicate_geometries_removed = 0;
    std::map<std::string, std::uint64_t> excluded_by_reason;
    std::map<std::string, std::uint64_t> output_by_class;
};

class OsmRoadPreprocessor {
public:
    [[nodiscard]] OsmPreprocessReport process(
        const std::string& input_path,
        const std::string& output_path,
        const OsmPreprocessOptions& options = {}) const;

    static void saveReport(
        const OsmPreprocessReport& report,
        const OsmPreprocessOptions& options,
        const std::string& input_path,
        const std::string& output_path,
        const std::string& report_path);
};

}  // namespace zeus::map
