#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "photonstack/DeepSkyWorkflow.hpp"

namespace photonstack {
struct DeepSkyRecipeField {
    std::string key, label, chineseLabel, group, type, defaultValue;
    double minimum = 0, maximum = 0;
    std::vector<std::string> choices;
};

struct DeepSkyRecipe {
    static constexpr unsigned version = 1;
    std::vector<std::filesystem::path> inputs;
    DeepSkyWorkflowOptions options;

    static std::vector<DeepSkyRecipeField> schema();
    // UTF-8, quoted scalar values, no shell evaluation. Relative input paths
    // resolve against the recipe directory. Unknown/duplicate keys are errors.
    static DeepSkyRecipe parse(const std::string& text, const std::filesystem::path& base);
    std::string serialize() const;
};
} // namespace photonstack
