#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "zeus/simulation/simulation_types.h"

namespace zeus::simulation {

// Structure-of-arrays vehicle storage for the hot simulation loop, following
// the architecture guideline for large vehicle counts (contiguous memory, no
// per-vehicle objects with virtual dispatch).
class VehicleStore {
public:
    void reserve(std::size_t count) {
        ids_.reserve(count);
        states_.reserve(count);
        route_ids_.reserve(count);
        route_indices_.reserve(count);
        offsets_.reserve(count);
        requested_departs_.reserve(count);
        actual_departs_.reserve(count);
        arrives_.reserve(count);
        traveled_.reserve(count);
        last_sample_ts_.reserve(count);
        held_.reserve(count);
        speed_factors_.reserve(count);
    }

    std::uint32_t append(const VehicleDemand& demand, std::uint32_t route_id) {
        const std::uint32_t id = static_cast<std::uint32_t>(ids_.size());
        ids_.push_back(id);
        states_.push_back(VehicleState::kWaiting);
        route_ids_.push_back(route_id);
        route_indices_.push_back(0);
        offsets_.push_back(0.0);
        requested_departs_.push_back(demand.depart_time_s);
        actual_departs_.push_back(std::numeric_limits<double>::quiet_NaN());
        arrives_.push_back(std::numeric_limits<double>::quiet_NaN());
        traveled_.push_back(0.0);
        last_sample_ts_.push_back(-1.0);
        held_.push_back(false);
        speed_factors_.push_back(1.0);
        return id;
    }

    std::size_t size() const { return ids_.size(); }

    std::vector<std::uint32_t> ids_;
    std::vector<VehicleState> states_;
    std::vector<std::uint32_t> route_ids_;
    std::vector<std::uint32_t> route_indices_;
    std::vector<double> offsets_;
    std::vector<double> requested_departs_;
    std::vector<double> actual_departs_;
    std::vector<double> arrives_;
    std::vector<double> traveled_;
    std::vector<double> last_sample_ts_;
    std::vector<bool> held_;
    std::vector<double> speed_factors_;
};

}  // namespace zeus::simulation
