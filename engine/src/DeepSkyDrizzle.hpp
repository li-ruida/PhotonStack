#pragma once
#include "photonstack/DeepSkyWorkflow.hpp"

namespace photonstack::detail {
// Validates metadata even for analysis-only runs. No pixel decoding or mutation.
void validateDrizzleSource(const std::filesystem::path& path,
                          const std::filesystem::path& reference);
ImageBuffer combineCfaDrizzle(const std::vector<std::size_t>& selected,
    const DeepSkyWorkflowOptions& options, const std::filesystem::path& directory,
    DeepSkyWorkflowResult& result, const DeepSkyProgress& progress);
}
