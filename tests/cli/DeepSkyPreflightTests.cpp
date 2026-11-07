#include "DeepSkyPreflight.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FileDigest.hpp"
#include <chrono>
#include <fstream>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace fs = std::filesystem;
void require(bool value, const std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("Preflight check failed at " + std::to_string(location.line()));
}
int main() {
    const auto root = fs::temp_directory_path() / ("photonstack-preflight-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    ImageBuffer image; image.width = 256; image.height = 128;
    image.pixels.resize(image.sampleCount(), 1);
    DeepSkyRecipe recipe;
    for (unsigned i = 0; i < 3; ++i) {
        const auto path = root / ("light-" + std::to_string(i) + ".fits");
        require(FitsCodec().write(image, path).ok); recipe.inputs.push_back(path);
    }
    const auto digest = fileSHA256(recipe.inputs[0]);
    const auto mtime = fs::last_write_time(recipe.inputs[0]);
    const auto target = root / "does-not-exist" / "run";
    const auto plan = inspectDeepSkyPlan(recipe, target);
    require(plan.lightCount == 3 && plan.inspectedCount == 3 && plan.issues.empty());
    require(plan.width == 256 && plan.height == 128 && plan.availableBytes.has_value());
    require(plan.plannedDiskBytes && plan.temporaryBytes && plan.productBytes && plan.stackBufferBytes);
    require(*plan.temporaryBytes > 3 * image.sampleCount() * sizeof(float));
    require(*plan.productBytes >= 4 * image.sampleCount() * sizeof(float));
    require(*plan.plannedDiskBytes > *plan.temporaryBytes + *plan.productBytes);
    require(*plan.assessmentSampleBytes == 3 * 192 * 128 * 8);
    require(!fs::exists(root / "does-not-exist"));
    require(fileSHA256(recipe.inputs[0]) == digest && fs::last_write_time(recipe.inputs[0]) == mtime);
    // Planning can inspect an existing nonempty working root without deleting
    // it or requiring a future UUID run directory to exist.
    require(inspectDeepSkyPlan(recipe, root).availableBytes.has_value());
    const auto calibration = root / "dark.fits";
    require(FitsCodec().write(image, calibration).ok);
    recipe.options.darks = {calibration};
    const auto calibrated = inspectDeepSkyPlan(recipe, target);
    require(calibrated.calibrationCount == 1 && calibrated.inspectedCount == 4);
    require(*calibrated.productBytes > *plan.productBytes);
    require(*calibrated.stackBufferBytes > *plan.stackBufferBytes);
    recipe.options.darks = {recipe.inputs[0]};
    const auto duplicate = inspectDeepSkyPlan(recipe, target);
    require(!duplicate.plannedDiskBytes && duplicate.issues[0].code == "DuplicateInput");
    recipe.options.darks.clear();
    recipe.inputs.push_back(root / "missing.fits");
    const auto missing = inspectDeepSkyPlan(recipe, target);
    require(!missing.plannedDiskBytes && missing.inspectedCount == 3 && missing.issues[0].code == "InputUnavailable");
    recipe.inputs.pop_back();
    image.width = 128; image.pixels.resize(image.sampleCount());
    const auto small = root / "small.fits";
    require(FitsCodec().write(image, small).ok);
    recipe.inputs.push_back(small);
    const auto mismatch = inspectDeepSkyPlan(recipe, target);
    require(!mismatch.plannedDiskBytes && mismatch.issues[0].code == "GeometryMismatch");
    recipe.inputs.pop_back();
    recipe.options.sensorPattern = true;
    const auto sensor = inspectDeepSkyPlan(recipe, target);
    require(sensor.additionalSensorStorage && !sensor.plannedDiskBytes && sensor.temporaryBytes);
    require(sensor.json().find("\"plannedDiskBytes\":null") != std::string::npos);
    require(sensor.issues[0].code == "SensorStorageUnestimated");
    require(!fs::exists(target));
}
