#pragma once

#include <vector>

#include "zeus/map/types.h"

namespace zeus::map {

class MapValidator {
public:
    [[nodiscard]] ValidationReport validate(
        const MapData& map,
        std::vector<ValidationIssue> prior_issues = {}) const;
};

[[nodiscard]] const char* severityName(IssueSeverity severity);

}  // namespace zeus::map

