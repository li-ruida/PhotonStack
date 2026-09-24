#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include "photonstack/FitsCodec.hpp"
#include "photonstack/MasterFrameBuilder.hpp"
#include "photonstack/Stacker.hpp"
#include "photonstack/Stretch.hpp"

namespace {
void require(bool condition, const std::source_location location = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("FITS Bayer regression failed at line " + std::to_string(location.line()));
}

void addCard(const std::filesystem::path& path, const std::string& key, const std::string& value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    std::string card(80, ' ');
    std::streamoff offset = 0;
    while (file.read(card.data(), 80)) {
        if (card.substr(0, 8) == "END     ")
            break;
        offset += 80;
    }
    require(static_cast<bool>(file));
    std::string added = key;
    added.resize(8, ' ');
    added += "= " + value;
    added.resize(80, ' ');
    file.seekp(offset);
    file.write(added.data(), 80);
    file.write(card.data(), 80);
}
void testParallelBayerTiles(const std::filesystem::path& path) {
    photonstack::ImageBuffer raw;
    raw.width = 1024; raw.height = 1027; raw.channels = 1;
    raw.format = photonstack::PixelFormat::Float32Gray;
    raw.pixels.resize(raw.pixelCount());
    for (std::size_t p = 0; p < raw.pixelCount(); ++p)
        raw.pixels[p] = p % 7919 == 0 ? std::numeric_limits<float>::quiet_NaN()
                                    : static_cast<float>((p * 1237) % 7919) / 7.0F - 40.0F;
    const photonstack::FitsCodec codec;
    for (const std::string pattern : {"RGGB", "GRBG", "GBRG", "BGGR"}) {
        require(codec.write(raw, path).ok);
        addCard(path, "BAYERPAT", "'" + pattern + "'");
        addCard(path, "XBAYROFF", "-1"); addCard(path, "YBAYROFF", "3");
        for (auto algorithm : {photonstack::FitsDemosaic::Bilinear, photonstack::FitsDemosaic::Malvar}) {
            const photonstack::FitsDecodeOptions options{.mode = photonstack::FitsDecodeMode::Scientific,
                .maskNonFinitePixels = true, .debayer = true, .demosaic = algorithm,
                .cfaInterpolationGains = {.68F, 1, 1.03F}};
            const auto parallel = codec.read(path, options);
            require(parallel.ok);
            for (unsigned first = 0; first < raw.height; first += 101) {
                const unsigned end = std::min(raw.height, first + 101);
                const unsigned top = first < 2 ? 0 : first - 2;
                const unsigned bottom = std::min(raw.height, end + 2);
                photonstack::ImageBuffer tile;
                tile.width = raw.width; tile.height = bottom - top; tile.channels = 1;
                tile.format = photonstack::PixelFormat::Float32Gray;
                tile.pixels.assign(raw.pixels.begin() + top * raw.width, raw.pixels.begin() + bottom * raw.width);
                const auto tilePath = path.string() + ".tile.fits";
                require(codec.write(tile, tilePath).ok);
                addCard(tilePath, "BAYERPAT", "'" + pattern + "'");
                addCard(tilePath, "XBAYROFF", "-1"); addCard(tilePath, "YBAYROFF", std::to_string(3 + top));
                const auto serial = codec.read(tilePath, options); // below parallel threshold
                require(serial.ok);
                for (unsigned y = first; y < end; ++y)
                    for (std::size_t x = 0; x < raw.width * 4; ++x) {
                        const auto a = parallel.image.pixels[y * raw.width * 4 + x];
                        const auto b = serial.image.pixels[(y - top) * raw.width * 4 + x];
                        require(a == b || (std::isnan(a) && std::isnan(b)));
                    }
                std::filesystem::remove(tilePath);
            }
        }
    }
}
} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "photonstack-bayer-regression.fits";
    const photonstack::FitsCodec codec;
    testParallelBayerTiles(path);
    for (const std::string pattern : {"RGGB", "GRBG", "GBRG", "BGGR"}) {
        for (int offset = 0; offset < 4; ++offset) {
            photonstack::ImageBuffer raw;
            raw.width = 6;
            raw.height = 6;
            raw.channels = 1;
            raw.format = photonstack::PixelFormat::Float32Gray;
            raw.colorEncoding = photonstack::ColorEncoding::Linear;
            for (int y = 0; y < 6; ++y) {
                for (int x = 0; x < 6; ++x) {
                    const char c = pattern[((y + offset / 2) % 2) * 2 + (x + offset % 2) % 2];
                    raw.pixels.push_back(c == 'R' ? 1000.0F : c == 'G' ? 2000.0F : 3000.0F);
                }
            }
            require(codec.write(raw, path).ok);
            addCard(path, "BAYERPAT", "'" + pattern + "'");
            addCard(path, "XBAYROFF", std::to_string(offset % 2));
            addCard(path, "YBAYROFF", std::to_string(offset / 2));
            auto decoded = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific, .debayer = true});
            require(decoded.ok);
            for (std::size_t i = 0; i < 36; ++i) {
                require(decoded.image.pixels[i * 4] == 1000.0F);
                require(decoded.image.pixels[i * 4 + 1] == 2000.0F);
                require(decoded.image.pixels[i * 4 + 2] == 3000.0F);
                require(decoded.image.pixels[i * 4 + 3] == 1.0F);
            }
            const auto malvar = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific,
                                                  .debayer = true,
                                                  .demosaic = photonstack::FitsDemosaic::Malvar});
            require(malvar.ok && malvar.image.pixels == decoded.image.pixels);
            // Calibration must see sensor samples before interpolation. A dark
            // with CFA-dependent values gives distinct RGB subtractions.
            auto dark = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific});
            require(dark.ok);
            for (std::size_t p = 0; p < dark.image.pixelCount(); ++p)
                for (int c = 0; c < 3; ++c) dark.image.pixels[p * 4 + c] *= .1F;
            photonstack::CalibrationOptions calibration;
            calibration.dark = &dark.image;
            calibration.clampNegativeValues = false;
            const auto calibrated = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific,
                .debayer = true, .demosaic = photonstack::FitsDemosaic::Malvar}, &calibration);
            require(calibrated.ok);
            for (std::size_t p = 0; p < calibrated.image.pixelCount(); ++p)
                for (int c = 0; c < 3; ++c) require(std::abs(calibrated.image.pixels[p * 4 + c] - decoded.image.pixels[p * 4 + c] * .9F) < .01F);
            const auto menon = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific,
                                                 .debayer = true,
                                                 .demosaic = photonstack::FitsDemosaic::Menon,
                                                 .cfaInterpolationGains = {.68F, 1, 1.03F}});
            require(menon.ok);
            for (std::size_t i = 0; i < decoded.image.sampleCount(); ++i)
                require(std::fabs(menon.image.pixels[i] - decoded.image.pixels[i]) < .002F);
            const auto balanced = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific,
                                                    .debayer = true,
                                                    .demosaic = photonstack::FitsDemosaic::Malvar,
                                                    .cfaInterpolationGains = {.68F, 1, 1.03F}});
            require(balanced.ok);
            for (std::size_t i = 0; i < decoded.image.sampleCount(); ++i)
                require(std::fabs(balanced.image.pixels[i] - decoded.image.pixels[i]) < .001F);
            const auto untouched = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific});
            require(untouched.ok);
            for (std::size_t i = 0; i < 36; ++i)
                require(untouched.image.pixels[i * 4] == raw.pixels[i]);
            const auto master = photonstack::MasterFrameBuilder().build({path, path}, {});
            require(master.ok && master.image.pixels == untouched.image.pixels);
            for (const auto method : {photonstack::StackMethod::Average, photonstack::StackMethod::SigmaClip}) {
                const auto stacked = photonstack::Stacker().stack({path, path}, {.method = method});
                require(stacked.ok);
                require(stacked.image.pixels == decoded.image.pixels);
            }
            // RGB output must not be demosaiced again on the next read.
            require(codec.write(decoded.image, path).ok);
            const auto rgb = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific, .debayer = true});
            require(rgb.ok && rgb.image.pixels == decoded.image.pixels);
        }
    }
    // A sampled achromatic PSF has known ground truth at every pixel. Check
    // all CFA phases, reconstruction error, measured samples and ADU offsets.
    for (const std::string pattern : {"RGGB", "GRBG", "GBRG", "BGGR"}) {
        photonstack::ImageBuffer raw;
        raw.width = raw.height = 33;
        raw.channels = 1;
        raw.format = photonstack::PixelFormat::Float32Gray;
        raw.colorEncoding = photonstack::ColorEncoding::Linear;
        for (int y = 0; y < 33; ++y)
            for (int x = 0; x < 33; ++x)
                raw.pixels.push_back(
                    -20.0F + 800.0F * std::exp(-((x - 16.3F) * (x - 16.3F) + (y - 15.7F) * (y - 15.7F)) / 4.5F));
        require(codec.write(raw, path).ok);
        addCard(path, "BAYERPAT", "'" + pattern + "'");
        auto opts = photonstack::FitsDecodeOptions{.mode = photonstack::FitsDecodeMode::Scientific, .debayer = true};
        const auto bilinear = codec.read(path, opts);
        opts.demosaic = photonstack::FitsDemosaic::Malvar;
        const auto malvar = codec.read(path, opts);
        auto directionalOptions = opts;
        directionalOptions.demosaic = photonstack::FitsDemosaic::Menon;
        const auto directionalBase = codec.read(path, directionalOptions);
        require(directionalBase.ok);
        require(bilinear.ok && malvar.ok);
        double oldError = 0, newError = 0;
        for (int y = 2; y < 31; ++y) {
            for (int x = 2; x < 31; ++x) {
                const auto pixel = y * 33 + x;
                const char c = pattern[(y % 2) * 2 + x % 2];
                require(malvar.image.pixels[pixel * 4 + (c == 'R' ? 0 : c == 'G' ? 1 : 2)] == raw.pixels[pixel]);
                require(directionalBase.image.pixels[pixel * 4 + (c == 'R'   ? 0
                                                                  : c == 'G' ? 1
                                                                             : 2)] == raw.pixels[pixel]);
                for (int channel = 0; channel < 3; ++channel) {
                    oldError += std::pow(bilinear.image.pixels[pixel * 4 + channel] - raw.pixels[pixel], 2);
                    newError += std::pow(malvar.image.pixels[pixel * 4 + channel] - raw.pixels[pixel], 2);
                }
            }
        }
        require(newError < oldError * .6);
        for (auto& v : raw.pixels)
            v += 1000;
        require(codec.write(raw, path).ok);
        addCard(path, "BAYERPAT", "'" + pattern + "'");
        const auto shifted = codec.read(path, opts);
        const auto directionalShifted = codec.read(path, directionalOptions);
        require(directionalShifted.ok);
        require(shifted.ok);
        for (std::size_t i = 0; i < raw.pixels.size(); ++i)
            for (int c = 0; c < 3; ++c)
                require(std::fabs(shifted.image.pixels[i * 4 + c] - malvar.image.pixels[i * 4 + c] - 1000) < .001F);
        for (std::size_t i = 0; i < raw.pixels.size(); ++i)
            for (int c = 0; c < 3; ++c)
                require(std::fabs(directionalShifted.image.pixels[i * 4 + c] - directionalBase.image.pixels[i * 4 + c] -
                                  1000) < .001F);
    }
    // Different channel responses on the same known stellar PSF must not
    // become different shapes. Interpolation gains are undone after the
    // reconstruction, preserving physical units and all measured samples.
    for (const std::string pattern : {"RGGB", "GRBG", "GBRG", "BGGR"}) {
        photonstack::ImageBuffer raw;
        raw.width = raw.height = 33;
        raw.channels = 1;
        raw.format = photonstack::PixelFormat::Float32Gray;
        raw.colorEncoding = photonstack::ColorEncoding::Linear;
        const std::array<float, 3> response{1.5F, 1.0F, .8F};
        std::vector<float> truth;
        for (int y = 0; y < 33; ++y)
            for (int x = 0; x < 33; ++x) {
                const float signal = 800.0F * std::exp(-((x - 16.3F) * (x - 16.3F) + (y - 15.7F) * (y - 15.7F)) / 4.5F);
                const char c = pattern[(y % 2) * 2 + x % 2];
                const int channel = c == 'R' ? 0 : c == 'G' ? 1 : 2;
                // Different additive sky levels do not share the stellar response.
                raw.pixels.push_back(-20.0F + channel * 15.0F + response[channel] * signal);
                for (int target = 0; target < 3; ++target)
                    truth.push_back(-20.0F + target * 15.0F + response[target] * signal);
            }
        require(codec.write(raw, path).ok);
        addCard(path, "BAYERPAT", "'" + pattern + "'");
        auto options = photonstack::FitsDecodeOptions{.mode = photonstack::FitsDecodeMode::Scientific,
                                                      .debayer = true,
                                                      .demosaic = photonstack::FitsDemosaic::Malvar};
        const auto unbalanced = codec.read(path, options);
        options.cfaInterpolationGains = {1 / response[0], 1, 1 / response[2]};
        const auto balanced = codec.read(path, options);
        require(unbalanced.ok && balanced.ok);
        double before = 0, after = 0;
        for (int y = 2; y < 31; ++y)
            for (int x = 2; x < 31; ++x) {
                const auto pixel = y * 33 + x;
                const char c = pattern[(y % 2) * 2 + x % 2];
                const int measured = c == 'R' ? 0 : c == 'G' ? 1 : 2;
                require(balanced.image.pixels[pixel * 4 + measured] == raw.pixels[pixel]);
                for (int channel = 0; channel < 3; ++channel) {
                    before += std::pow((unbalanced.image.pixels[pixel * 4 + channel] - truth[pixel * 3 + channel]) /
                                           response[channel],
                                       2);
                    after += std::pow((balanced.image.pixels[pixel * 4 + channel] - truth[pixel * 3 + channel]) /
                                          response[channel],
                                      2);
                }
            }
        require(after < before * .7);
        options.cfaInterpolationGains[0] = 0;
        require(!codec.read(path, options).ok);
        options.cfaInterpolationGains[0] = std::numeric_limits<float>::infinity();
        require(!codec.read(path, options).ok);
    }
    // Independent boundary fixture from Colour - Demosaicing 0.2.7's
    // documented Menon2007 example (BSD-3-Clause, see third_party notice).
    photonstack::ImageBuffer tiny;
    tiny.width = 4;
    tiny.height = 2;
    tiny.channels = 1;
    tiny.format = photonstack::PixelFormat::Float32Gray;
    tiny.colorEncoding = photonstack::ColorEncoding::Linear;
    tiny.pixels = {.30980393F, .36078432F, .30588236F, .3764706F, .35686275F, .39607844F, .36078432F, .40000001F};
    require(codec.write(tiny, path).ok);
    addCard(path, "BAYERPAT", "'RGGB'");
    const auto directional = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific,
                                               .debayer = true,
                                               .demosaic = photonstack::FitsDemosaic::Menon});
    const float expected[] = {.30980393F, .35686275F, .39215687F, .30980393F, .36078432F, .39607844F,
                              .30588236F, .36078432F, .39019608F, .32156864F, .3764706F,  .40000001F,
                              .30980393F, .35686275F, .39215687F, .30980393F, .36078432F, .39607844F,
                              .30588236F, .36078432F, .39019608F, .32156864F, .3764706F,  .40000001F};
    require(directional.ok);
    for (int p = 0; p < 8; ++p)
        for (int c = 0; c < 3; ++c)
            if (std::fabs(directional.image.pixels[p * 4 + c] - expected[p * 3 + c]) >= 2.e-7F)
                throw std::runtime_error("Menon fixture pixel " + std::to_string(p) + " channel " + std::to_string(c) +
                                         " actual " + std::to_string(directional.image.pixels[p * 4 + c]) +
                                         " expected " + std::to_string(expected[p * 3 + c]));
    // A masked CFA defect cannot become a zero-valued MHC neighbour in
    // display-normalized decoding. Its neighbours fall back safely.
    photonstack::ImageBuffer defective;
    defective.width = defective.height = 9;
    defective.channels = 1;
    defective.pixels.assign(81, 1000.0F);
    defective.pixels[0] = 0;
    defective.pixels[40] = std::numeric_limits<float>::quiet_NaN();
    require(codec.write(defective, path).ok);
    addCard(path, "BAYERPAT", "'GRBG'");
    const auto masked = codec.read(path, {.mode = photonstack::FitsDecodeMode::DisplayNormalized,
                                          .maskNonFinitePixels = true,
                                          .debayer = true,
                                          .demosaic = photonstack::FitsDemosaic::Malvar});
    require(masked.ok && masked.image.pixels[40 * 4 + 3] == 0);
    for (int c = 0; c < 3; ++c)
        require(masked.image.pixels[41 * 4 + c] == 1);
    const auto directionalMasked = codec.read(path, {.mode = photonstack::FitsDecodeMode::DisplayNormalized,
                                                     .maskNonFinitePixels = true,
                                                     .debayer = true,
                                                     .demosaic = photonstack::FitsDemosaic::Menon});
    require(directionalMasked.ok && directionalMasked.image.pixels[40 * 4 + 3] == 0);
    for (int c = 0; c < 3; ++c)
        require(directionalMasked.image.pixels[41 * 4 + c] == 1);
    photonstack::ImageBuffer zeroDark;
    zeroDark.width = zeroDark.height = 9; zeroDark.channels = 4;
    zeroDark.pixels.assign(81 * 4, 0);
    for (int p = 0; p < 81; ++p) zeroDark.pixels[p * 4 + 3] = 1;
    photonstack::CalibrationOptions maskedCalibration;
    maskedCalibration.dark = &zeroDark; maskedCalibration.clampNegativeValues = false;
    const auto calibratedMasked = codec.read(path, {.mode = photonstack::FitsDecodeMode::Scientific,
        .maskNonFinitePixels = true, .debayer = true, .demosaic = photonstack::FitsDemosaic::Malvar}, &maskedCalibration);
    require(calibratedMasked.ok && calibratedMasked.image.pixels[40 * 4 + 3] == 0);
    for (int c = 0; c < 3; ++c) require(calibratedMasked.image.pixels[41 * 4 + c] == 1000);
    // An additive sky change must not alter stellar contrast or scientific range.
    photonstack::ImageBuffer base;
    base.width = 8;
    base.height = 8;
    base.channels = 4;
    base.colorEncoding = photonstack::ColorEncoding::Linear;
    base.pixels.resize(8 * 8 * 4);
    for (std::size_t i = 0; i < 64; ++i) {
        for (int c = 0; c < 3; ++c)
            base.pixels[i * 4 + c] = 1000.0F + c * 20 + (i == 20 ? 500.0F : 0.0F);
        base.pixels[i * 4 + 3] = 1.0F;
    }
    auto shifted = base;
    for (std::size_t i = 0; i < 64; ++i) {
        for (int c = 0; c < 3; ++c)
            shifted.pixels[i * 4 + c] += 100.0F + c * 30;
    }
    const auto otherPath = path.parent_path() / "photonstack-background-regression.fits";
    require(codec.write(base, path).ok && codec.write(shifted, otherPath).ok);
    for (const auto method : {photonstack::StackMethod::Average, photonstack::StackMethod::SigmaClip}) {
        photonstack::StackOptions options;
        options.method = method;
        options.normalizeBackground = true;
        const auto matched = photonstack::Stacker().stack({path, otherPath}, options);
        require(matched.ok && matched.image.pixels == base.pixels);
        options.normalizeBackground = false;
        const auto unmatched = photonstack::Stacker().stack({path, otherPath}, options);
        require(unmatched.ok && unmatched.image.pixels[0] == 1050.0F);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(otherPath);

    // Signed, background-subtracted data must stretch identically to the same
    // signal on an ADU pedestal, with no near-zero luminance chroma explosion.
    photonstack::ImageBuffer signedSky;
    signedSky.width = 1;
    signedSky.height = 1;
    signedSky.channels = 4;
    signedSky.pixels = {10.0F, -2.0F, -8.0F, 1.0F};
    auto pedestalSky = signedSky;
    for (int c = 0; c < 3; ++c)
        pedestalSky.pixels[c] += 1000.0F;
    const auto signedStretch = photonstack::Stretch().apply(signedSky, {.blackPoint = -100.0F, .whitePoint = 900.0F});
    const auto pedestalStretch =
        photonstack::Stretch().apply(pedestalSky, {.blackPoint = 900.0F, .whitePoint = 1900.0F});
    require(signedStretch.ok && pedestalStretch.ok);
    for (int c = 0; c < 3; ++c) {
        require(std::fabs(signedStretch.image.pixels[c] - pedestalStretch.image.pixels[c]) < 1.0e-6F);
        require(signedStretch.image.pixels[c] < 0.12F);
    }
}
