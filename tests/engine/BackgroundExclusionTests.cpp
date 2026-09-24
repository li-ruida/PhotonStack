#include "photonstack/BackgroundExtractor.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace photonstack;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    // A large exclusion leaves only four bright corner pixels in one cell.
    // Those pixels must not become a background pedestal under the target.
    ImageBuffer image;
    image.width = image.height = 128;
    image.channels = 4;
    image.colorEncoding = ColorEncoding::Linear;
    image.pixels.resize(image.sampleCount());
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
        const double radius = std::hypot(x - 47.5, y - 47.5);
        const bool source = radius < 14;
        const bool remnant = x >= 32 && x < 64 && y >= 32 && y < 64 && radius > 21.5;
        for (int c = 0; c < 3; ++c)
            image.pixels[(y * 128 + x) * 4 + c] = (c + 1) * (10 + (source ? 50 : 0) + (remnant ? 350 : 0));
        image.pixels[(y * 128 + x) * 4 + 3] = 1;
    }
    for (bool protect : {true, false}) {
        BackgroundGridOptions options;
        options.columns = options.rows = 4;
        options.extraction.clampOutput = false;
        options.extraction.preserveBrightness = false;
        options.protectBrightTargets = protect;
        options.exclusions = {{47.5F, 47.5F, 21.5F, 21.5F, 0}};
        auto result = BackgroundExtractor().extractGrid(image, options);
        require(result.ok, "Exclusion should leave enough sky for a model");
        for (int c = 0; c < 3; ++c) {
            const float center = result.image.pixels[(48 * 128 + 48) * 4 + c];
            std::cout << "protect=" << protect << " channel=" << c << " target=" << center << '\n';
            require(std::abs(center - 50 * (c + 1)) < .001,
                    "A tiny bright remnant contaminated the background beneath an excluded target");
            require(std::abs(result.image.pixels[(70 * 128 + 48) * 4 + c]) < .001,
                    "Excluded cell contaminated neighboring sky");
        }
        require(result.sampledGridCells == 15 && result.filledGridCells == 1,
                "Unsupported cell was reported as a real background measurement");
        require(result.image.pixels[(48 * 128 + 48) * 4 + 3] == 1,
                "Background correction changed target coverage");

        options.exclusions = {{64, 64, 1000, 1000, 0}};
        auto noSky = BackgroundExtractor().extractGrid(image, options);
        require(!noSky.ok && noSky.errorCode == "BackgroundSamplesInsufficient",
                "A wholly excluded field must not invent a background model");
    }
}
