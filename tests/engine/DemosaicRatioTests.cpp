#include "photonstack/FitsCodec.hpp"
#include "photonstack/DeepSkyRecipe.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace photonstack;
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void tag(const std::filesystem::path& path, const std::string& pattern, int dx, int dy) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    std::string end(80, ' ');
    std::streamoff offset = 0;
    while (file.read(end.data(), 80) && end.substr(0, 8) != "END     ") offset += 80;
    require(static_cast<bool>(file), "FITS header missing END");
    file.seekp(offset);
    for (std::string card : {"BAYERPAT= '" + pattern + "'", "XBAYROFF= " + std::to_string(dx),
                            "YBAYROFF= " + std::to_string(dy)}) {
        card.resize(80, ' ');
        file.write(card.data(), 80);
    }
    file.write(end.data(), 80);
}
struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("photonstack-ratio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() { require(std::filesystem::create_directory(path), "test directory exists"); }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}

int main() {
    Temporary temporary;
    const auto path = temporary.path / "fixture.fits";
    const FitsCodec codec;
    for (const std::string pattern : {"RGGB", "GRBG", "GBRG", "BGGR"})
        for (int dx : {-1, 0, 1}) for (int dy : {-1, 0, 1})
            for (int side : {2, 11}) for (bool hole : {false, true})
                for (auto gains : {std::array<float, 3>{1, 1, 1}, {.7F, 1, 1.03F}, {2, .5F, 1.4F}}) {
                    ImageBuffer raw;
                    raw.width = side; raw.height = side == 2 ? 2 : 9; raw.channels = 1;
                    raw.format = PixelFormat::Float32Gray; raw.colorEncoding = ColorEncoding::Linear;
                    raw.pixels.resize(raw.pixelCount());
                    const auto channel = [&](int x, int y) {
                        const auto c = pattern[((y + dy + 2) % 2) * 2 + (x + dx + 2) % 2];
                        return c == 'R' ? 0 : c == 'G' ? 1 : 2;
                    };
                    const std::array<float, 3> values{-7.5F, 3.25F, 12.75F};
                    for (unsigned y = 0; y < raw.height; ++y) for (int x = 0; x < side; ++x)
                        raw.pixels[y * side + x] = hole && side > 2 && x == 5 && y == 4
                            ? std::numeric_limits<float>::quiet_NaN() : values[channel(x, y)];
                    require(codec.write(raw, path).ok, "fixture write failed");
                    tag(path, pattern, dx, dy);
                    FitsDecodeOptions options;
                    options.mode = FitsDecodeMode::Scientific; options.debayer = true;
                    options.maskNonFinitePixels = true; options.demosaic = FitsDemosaic::Ratio;
                    options.cfaInterpolationGains = gains;
                    const auto result = codec.read(path, options);
                    require(result.ok, "ratio decode failed");
                    for (unsigned y = 0; y < raw.height; ++y) for (int x = 0; x < side; ++x) {
                        const auto p = y * side + x;
                        if (!std::isfinite(raw.pixels[p])) {
                            for (int c = 0; c < 4; ++c) require(result.image.pixels[4 * p + c] == 0, "mask lost");
                            continue;
                        }
                        require(result.image.pixels[4 * p + 3] == 1, "valid coverage changed");
                        require(result.image.pixels[4 * p + channel(x, y)] == raw.pixels[p], "native sample changed");
                        for (int c = 0; c < 3; ++c) {
                            const auto value = result.image.pixels[4 * p + c];
                            require(std::isfinite(value), "finite neighbour lost");
                            if (!hole) require(std::abs(value - values[c]) < .00002F, "constant physical colour changed");
                        }
                    }
                    std::filesystem::remove(path);
                }
    // Independently sampled coloured stars and a curved filament. Compare to
    // their known RGB values, not to a smoothed image or a noise-only score.
    for (auto gains : {std::array<float, 3>{1, 1, 1}, {.7F, 1, 1.03F}}) {
        double error[2]{};
        for (auto colour : {std::array<double, 3>{1, 1, 1}, {2, .6, .25}, {.5, .8, 2}})
            for (double phase : {.1, .35, .6, .85}) {
                ImageBuffer raw;
                raw.width = raw.height = 96; raw.channels = 1;
                raw.format = PixelFormat::Float32Gray; raw.colorEncoding = ColorEncoding::Linear;
                raw.pixels.resize(raw.pixelCount());
                const auto signal = [&](double x, double y, int c) {
                    x -= phase; y -= .8 - phase;
                    const double star = std::exp(-((x - 28) * (x - 28) + (y - 28) * (y - 28)) / (2 * 1.3 * 1.3));
                    const double d = x - 60 - 3 * std::sin(y / 12);
                    const double filament = std::exp(-d * d / 8 - (y - 56) * (y - 56) / 600);
                    return (1000 + 1000 * colour[c] * (star + filament)) / gains[c];
                };
                for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x) {
                    constexpr int colours[4]{1, 0, 2, 1};
                    raw.pixels[y * 96 + x] = static_cast<float>(signal(x, y, colours[(y % 2) * 2 + x % 2]));
                }
                require(codec.write(raw, path).ok, "signal fixture write failed");
                tag(path, "GRBG", 0, 0);
                for (int method = 0; method < 2; ++method) {
                    FitsDecodeOptions options;
                    options.mode = FitsDecodeMode::Scientific; options.debayer = true;
                    options.demosaic = method ? FitsDemosaic::Ratio : FitsDemosaic::Malvar;
                    options.cfaInterpolationGains = gains;
                    const auto result = codec.read(path, options);
                    require(result.ok, "signal decode failed");
                    for (int y = 16; y < 80; ++y) for (int x = 16; x < 80; ++x) for (int c = 0; c < 3; ++c) {
                        const double residual = (result.image.pixels[(y * 96 + x) * 4 + c] - signal(x, y, c)) * gains[c];
                        error[method] += residual * residual;
                    }
                }
                std::filesystem::remove(path);
            }
        require(error[1] < error[0] * .95, "known colour structure reconstruction regressed");
    }
    const std::string header = "PHOTONSTACK_DEEP_SKY_RECIPE 1\n";
    const auto ratio = DeepSkyRecipe::parse(header + "decode.demosaic \"ratio\"\n", temporary.path);
    require(ratio.serialize().find("decode.demosaic \"ratio\"") != std::string::npos, "recipe lost method");
    require(DeepSkyRecipe::parse(ratio.serialize(), temporary.path).serialize() == ratio.serialize(), "recipe roundtrip");
    require(FitsDecodeOptions{}.demosaic == FitsDemosaic::Bilinear, "codec default changed");
    require(DeepSkyRecipe{}.serialize().find("decode.demosaic \"malvar\"") != std::string::npos, "workflow default changed");
}
