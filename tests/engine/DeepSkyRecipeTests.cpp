#include "photonstack/DeepSkyRecipe.hpp"
#include <set>
#include <stdexcept>
#include <source_location>
using namespace photonstack;
void require(bool value, const std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Recipe test failed at " + std::to_string(where.line()));
}
bool invalid(const std::string& text) {
    try { DeepSkyRecipe::parse(text, "/tmp"); return false; } catch (const std::invalid_argument&) { return true; }
}
int main() {
    DeepSkyRecipe recipe;
    recipe.inputs = {"/tmp/亮场 one.fit", "/tmp/quote\" and \\slash.fit", "/tmp/three.fit"};
    recipe.options.quality.overrides = {FrameSelectionOverride::Keep, FrameSelectionOverride::Reject, FrameSelectionOverride::Automatic};
    recipe.options.darks = {"/tmp/dark \"quoted\".fit", "/tmp/dark two.fit"};
    recipe.options.backgroundDenoiseStrength = .75;
    const auto text = recipe.serialize();
    const auto parsed = DeepSkyRecipe::parse(text, "/tmp");
    require(parsed.inputs == recipe.inputs);
    require(parsed.options.quality.overrides == recipe.options.quality.overrides);
    require(parsed.options.darks == recipe.options.darks);
    require(parsed.options.backgroundDenoiseStrength == .75);
    require(!parsed.options.sensorPattern);
    require(!parsed.options.saveDenoiseStages);
    require(parsed.serialize() == text);
    recipe.options.noiseModel = DeepSkyNoiseModel::IndependentLuminance;
    require(DeepSkyRecipe::parse(recipe.serialize(), "/tmp").options.noiseModel == DeepSkyNoiseModel::IndependentLuminance);
    require(parsed.options.noiseModel == DeepSkyNoiseModel::Scene);
    std::set<std::string> keys;
    for (const auto& field : DeepSkyRecipe::schema()) {
        require(keys.insert(field.key).second);
        require(!field.label.empty() && !field.chineseLabel.empty());
        require(text.find(field.key + " ") != std::string::npos);
    }
    const std::string header = "PHOTONSTACK_DEEP_SKY_RECIPE 1\n";
    require(!DeepSkyRecipe::parse(header, "/tmp").options.saveDenoiseStages);
    require(DeepSkyRecipe::parse(header + "output.save-denoise-stages \"on\"\n", "/tmp").options.saveDenoiseStages);
    auto diagnostic = recipe; diagnostic.options.saveDenoiseStages = true;
    require(DeepSkyRecipe::parse(diagnostic.serialize(), "/tmp").options.saveDenoiseStages);
    require(invalid(header + "output.save-denoise-stages \"yes\"\n"));
    require(invalid(header + "denoise.noise-model \"unknown\"\n"));
    require(DeepSkyRecipe::parse(header + "calibration.sensor-pattern \"on\"\n", "/tmp").options.sensorPattern);
    require(invalid(header + "calibration.sensor-pattern \"on\"\ndecode.debayer \"off\"\n"));
    require(invalid("PHOTONSTACK_DEEP_SKY_RECIPE 2\n"));
    require(invalid(header + "unknown \"on\"\n"));
    require(invalid(header + "decode.debayer \"yes\"\n"));
    require(invalid(header + "decode.demosaic \"magic\"\n"));
    require(invalid(header + "decode.debayer \"on\"\ndecode.debayer \"off\"\n"));
    for (auto bad : {"NaN", "inf", "-inf", "3junk", "", "1e999"})
        require(invalid(header + "selection.residual-sigma \"" + bad + "\"\n"));
    require(invalid(header + "selection.sample-side \"2.5\"\n"));
    require(invalid(header + "denoise.h \"-0.5\"\n"));
    require(invalid(header + "denoise.background-strength \"1.1\"\n"));
    require(invalid(header + "denoise.background-strength \"NaN\"\n"));
    require(DeepSkyRecipe::parse(header, "/tmp").options.backgroundDenoiseStrength == 0);
    require(invalid(header + "stack.method \"average\"\n"));
    require(invalid(header + "selection.overrides \"keep,reject\"\ninput \"one.fit\"\n"));
    const auto relative = DeepSkyRecipe::parse(header + "input \"one.fit\"\n", "/tmp/recipes");
    require(relative.inputs[0] == "/tmp/recipes/one.fit");
}
