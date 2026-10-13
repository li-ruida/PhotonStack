#include "photonstack/FitsCodec.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <source_location>
#include <stdexcept>

using namespace photonstack;
namespace {
void require(bool condition, const std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("CFA star guard regression at line " + std::to_string(location.line()));
}
void tag(const std::filesystem::path& path, const std::string& pattern, int dx, int dy) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    std::string end(80, ' ');
    std::streamoff offset = 0;
    while (file.read(end.data(), 80) && end.substr(0, 8) != "END     ") offset += 80;
    require(static_cast<bool>(file)); file.seekp(offset);
    for (std::string card : {"BAYERPAT= '" + pattern + "'", "XBAYROFF= " + std::to_string(dx),
                            "YBAYROFF= " + std::to_string(dy)}) {
        card.resize(80, ' '); file.write(card.data(), 80);
    }
    file.write(end.data(), 80);
}
struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("photonstack-star-guard-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".fits");
    ~Temporary() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
}

int main() {
    Temporary temporary;
    const FitsCodec codec;
    double error[2]{}, undershoot[2]{};
    std::size_t changed = 0;
    for (const std::string pattern : {"GRBG", "RGGB", "GBRG", "BGGR"})
        for (double sigma : {.9, 1.2, 1.6, 2.5})
            for (const auto color : {std::array<double, 3>{1, 1, 1}, {2, .6, .25}, {.5, .8, 2}}) {
                ImageBuffer raw;
                raw.width = raw.height = 96; raw.channels = 1; raw.format = PixelFormat::Float32Gray;
                raw.pixels.resize(raw.pixelCount());
                const auto channel = [&](int x, int y) {
                    const auto c = pattern[((y + 3) % 2) * 2 + (x + 1) % 2];
                    return c == 'R' ? 0 : c == 'G' ? 1 : 2;
                };
                const auto truth = [&](int x, int y, int c) {
                    return -200 + 20000 * color[c] * std::exp(-((x - 47.35) * (x - 47.35) +
                        (y - 48.6) * (y - 48.6)) / (2 * sigma * sigma));
                };
                for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x)
                    raw.pixels[y * 96 + x] = truth(x, y, channel(x, y));
                require(codec.write(raw, temporary.path).ok); tag(temporary.path, pattern, -1, 3);
                FitsDecodeOptions options{.mode = FitsDecodeMode::Scientific, .debayer = true,
                    .demosaic = FitsDemosaic::Malvar};
                const auto control = codec.read(temporary.path, options); require(control.ok);
                options.demosaic = FitsDemosaic::MalvarGuarded;
                const auto guarded = codec.read(temporary.path, options); require(guarded.ok);
                std::array<double, 3> delta{}, flux{};
                for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x) {
                    const auto p = y * 96 + x;
                    require(guarded.image.pixels[p * 4 + channel(x, y)] == raw.pixels[p]);
                    require(guarded.image.pixels[p * 4 + 3] == control.image.pixels[p * 4 + 3]);
                    for (int c = 0; c < 3; ++c) {
                        const double a = control.image.pixels[p * 4 + c], b = guarded.image.pixels[p * 4 + c];
                        delta[c] += b - a; flux[c] += a + 200; changed += a != b;
                        error[0] += std::pow(a - truth(x, y, c), 2);
                        error[1] += std::pow(b - truth(x, y, c), 2);
                        undershoot[0] += std::max(0., -200 - a);
                        undershoot[1] += std::max(0., -200 - b);
                    }
                }
                for (int c = 0; c < 3; ++c) require(std::abs(delta[c]) < std::abs(flux[c]) * 1.e-6);
                // Positive unit conversion and additive pedestal must not turn
                // valid negative scientific background into a zero clamp.
                for (auto& v : raw.pixels) v = v * .125F - 10000;
                require(codec.write(raw, temporary.path).ok); tag(temporary.path, pattern, -1, 3);
                const auto scaled = codec.read(temporary.path, options); require(scaled.ok);
                for (std::size_t p = 0; p < raw.pixelCount(); ++p) for (int c = 0; c < 3; ++c)
                    require(std::abs(scaled.image.pixels[p * 4 + c] -
                        (guarded.image.pixels[p * 4 + c] * .125F - 10000)) < .03F);
            }
    std::cout << "changed=" << changed << " known-truth-error-ratio=" << error[1] / error[0]
              << " undershoot-ratio=" << undershoot[1] / undershoot[0] << '\n';
    require(changed > 1000); require(error[1] < .95 * error[0]); require(undershoot[1] < .7 * undershoot[0]);
    // Sky-only noise and smooth extended objects must remain untouched.
    for (bool noise : {false, true}) {
        ImageBuffer raw;
        raw.width = raw.height = 96; raw.channels = 1; raw.format = PixelFormat::Float32Gray;
        raw.pixels.resize(raw.pixelCount());
        std::mt19937 random(518); std::normal_distribution<float> normal(0, 10);
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x)
            raw.pixels[y * 96 + x] = -100 + (noise ? normal(random) :
                300 * std::exp(-((x - 48.) * (x - 48.) + (y - 48.) * (y - 48.)) / 288));
        require(codec.write(raw, temporary.path).ok); tag(temporary.path, "GRBG", 0, 0);
        FitsDecodeOptions options{.mode = FitsDecodeMode::Scientific, .debayer = true, .demosaic = FitsDemosaic::Malvar};
        const auto a = codec.read(temporary.path, options); options.demosaic = FitsDemosaic::MalvarGuarded;
        const auto b = codec.read(temporary.path, options); require(a.ok && b.ok && a.image.pixels == b.image.pixels);
    }
    // Enough separated sources to exercise parallel fitting. A masked star
    // must remain unmodified; the other apertures still conserve color flux.
    {
        ImageBuffer raw;
        raw.width = raw.height = 896; raw.channels = 1; raw.format = PixelFormat::Float32Gray;
        raw.pixels.resize(raw.pixelCount());
        constexpr int colors[4]{1, 0, 2, 1};
        const std::array<float, 3> gains{.68F, 1, 1.03F};
        for (int y = 0; y < 896; ++y) for (int x = 0; x < 896; ++x) {
            const int c = colors[(y % 2) * 2 + x % 2];
            const double dx = x % 64 - 32.3, dy = y % 64 - 31.7;
            raw.pixels[y * 896 + x] = (-100 + 20000 * std::exp(-(dx * dx + dy * dy) / 2.88)) / gains[c];
        }
        raw.pixels[96 * 896 + 96] = std::numeric_limits<float>::quiet_NaN();
        require(codec.write(raw, temporary.path).ok); tag(temporary.path, "GRBG", 0, 0);
        FitsDecodeOptions options{.mode = FitsDecodeMode::Scientific, .maskNonFinitePixels = true,
            .debayer = true, .demosaic = FitsDemosaic::Malvar, .cfaInterpolationGains = gains};
        const auto a = codec.read(temporary.path, options); options.demosaic = FitsDemosaic::MalvarGuarded;
        const auto b = codec.read(temporary.path, options); require(a.ok && b.ok);
        double delta[3]{}; std::size_t edits = 0;
        for (int y = 0; y < 896; ++y) for (int x = 0; x < 896; ++x) {
            const auto p = std::size_t(y) * 896 + x;
            const int native = colors[(y % 2) * 2 + x % 2];
            require(a.image.pixels[p * 4 + native] == b.image.pixels[p * 4 + native]);
            require(a.image.pixels[p * 4 + 3] == b.image.pixels[p * 4 + 3]);
            for (int c = 0; c < 3; ++c) {
                const double difference = b.image.pixels[p * 4 + c] - a.image.pixels[p * 4 + c];
                delta[c] += difference; edits += difference != 0;
                if (std::abs(x - 96) < 20 && std::abs(y - 96) < 20) require(difference == 0);
            }
        }
        require(edits > 1000);
        for (int c = 0; c < 3; ++c) require(std::abs(delta[c]) < 1);
    }
}
