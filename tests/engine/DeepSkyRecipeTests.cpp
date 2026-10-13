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
    recipe.options.develop.starColorCleanup = .6F;
    const auto text = recipe.serialize();
    for (auto method : {StackMethod::Maximum, StackMethod::Minimum}) {
        auto extrema = recipe;
        extrema.options.stack.method = method;
        require(invalid(extrema.serialize())); // Default MAD/maps must not be silently ignored.
        extrema.options.stack.sigma.medianMad = false;
        extrema.options.stack.collectRejectionMaps = false;
        require(DeepSkyRecipe::parse(extrema.serialize(), "/tmp").options.stack.method == method);
        extrema.options.stack.frameWeights = {1,1,1};
        require(invalid(extrema.serialize()));
    }
    const auto parsed = DeepSkyRecipe::parse(text, "/tmp");
    require(parsed.inputs == recipe.inputs);
    require(parsed.options.quality.overrides == recipe.options.quality.overrides);
    require(parsed.options.darks == recipe.options.darks);
    require(parsed.options.backgroundDenoiseStrength == .75);
    require(parsed.options.develop.starColorCleanup == .6F);
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
    require(DeepSkyRecipe::parse(header, "/tmp").options.develop.starColorCleanup == 0);
    require(invalid(header + "develop.star-color-cleanup \"1.1\"\n"));
    require(invalid(header + "develop.star-color-cleanup \"NaN\"\n"));
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
    const auto guarded = DeepSkyRecipe::parse(header + "decode.demosaic \"malvar-guarded\"\n", "/tmp");
    require(guarded.options.stack.fitsDemosaic == FitsDemosaic::MalvarGuarded);
    require(DeepSkyRecipe::parse(guarded.serialize(), "/tmp").serialize() == guarded.serialize());
    require(DeepSkyRecipe{}.options.stack.fitsDemosaic == FitsDemosaic::MalvarGuarded);
    require(DeepSkyRecipe::parse(header, "/tmp").options.stack.fitsDemosaic == FitsDemosaic::Malvar);
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
