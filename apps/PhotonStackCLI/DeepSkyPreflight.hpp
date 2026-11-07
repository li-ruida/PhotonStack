#pragma once
#include "photonstack/DeepSkyRecipe.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct DeepSkyPreflightIssue {
    std::string code, path, message;
};

// Read-only planning information. Header inspection does not prove that every
// pixel can be decoded or that calibration/acquisition metadata are compatible.
struct DeepSkyPreflight {
    std::size_t lightCount = 0, calibrationCount = 0, inspectedCount = 0;
    std::uint32_t width = 0, height = 0;
    std::uint64_t sourceBytes = 0;
    std::optional<std::uint64_t> temporaryBytes, productBytes, plannedDiskBytes;
    std::optional<std::uint64_t> stackBufferBytes, assessmentSampleBytes, availableBytes;
    std::string destination;
    bool additionalSensorStorage = false;
    std::vector<DeepSkyPreflightIssue> issues;
    std::string json() const;
};

DeepSkyPreflight inspectDeepSkyPlan(const photonstack::DeepSkyRecipe& recipe,
                                  const std::filesystem::path& destination);
