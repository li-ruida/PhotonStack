#ifdef __APPLE__
#include "MetalCompute.hpp"
#endif
#include <bit>
#include "photonstack/StageProfile.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/CalibrationMetadata.hpp"
#include "photonstack/SensorMasterBuilder.hpp"
#include "photonstack/SensorCalibration.hpp"
#include "photonstack/FileDigest.hpp"
#include "DemosaicMenon.hpp"
#include "DemosaicRatio.hpp"
#include "DemosaicStarGuard.hpp"
#include "photonstack/ParallelRanges.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace photonstack {
namespace {

constexpr std::size_t kFitsBlockSize = 2880;
constexpr std::size_t kFitsCardSize = 80;
constexpr std::size_t kFitsStreamChunkBytes = 1024 * 1024;

struct FitsHeader {
    bool ok = false;
    std::map<std::string, std::string> values;
    std::map<std::string, std::string> rawCards;
    std::set<std::string> duplicateKeys;
    std::size_t dataOffset = 0;
    std::string errorCode;
    std::string message;
};

std::string lowercaseExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

std::string trim(std::string value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto end =
        std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::string parseCardValue(const std::string& card) {
    if (card.size() < 10 || card[8] != '=') {
        return {};
    }

    std::string value = card.substr(10);
    bool inString = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'') {
            inString = !inString;
        }
        if (!inString && value[i] == '/') {
            value.resize(i);
            break;
        }
    }

    value = trim(value);
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        value = value.substr(1, value.size() - 2);
    }
    return trim(value);
}

FitsHeader parseHeader(std::ifstream& file, bool acquisition = false) {
    FitsHeader header;
    file.clear();
    file.seekg(0, std::ios::beg);
    if (!file) {
        header.errorCode = "InputReadFailed";
        header.message = "FITS file could not be read";
        return header;
    }

    std::array<char, kFitsBlockSize> block{};
    std::size_t blockOffset = 0;
    while (true) {
        if (acquisition && blockOffset > 1024 * 1024 - kFitsBlockSize) {
            header.message = "Calibration metadata header exceeds 1 MiB";
            header.errorCode = "ImageHeaderInvalid";
            return header;
        }
        file.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto bytesRead = file.gcount();
        if (bytesRead != static_cast<std::streamsize>(block.size())) {
            if (blockOffset == 0) {
                header.errorCode = "InputFormatUnsupported";
                header.message = "FITS file is smaller than one header block";
            } else {
                header.errorCode = "ImageHeaderInvalid";
                header.message = "FITS header is missing END card";
            }
            return header;
        }

        for (std::size_t offset = 0; offset < block.size(); offset += kFitsCardSize) {
            const std::string card(block.data() + offset, kFitsCardSize);
            const auto key = trim(card.substr(0, 8));
            if (key == "END") {
                if (blockOffset > std::numeric_limits<std::size_t>::max() - kFitsBlockSize) {
                    header.errorCode = "ImageHeaderInvalid";
                    header.message = "FITS header exceeds supported file offsets";
                    return header;
                }
                header.ok = true;
                header.dataOffset = blockOffset + kFitsBlockSize;
                return header;
            }
            if (!key.empty() && card[8] == '=') {
                if (acquisition) {
                    if (header.values.contains(key)) header.duplicateKeys.insert(key);
                    header.rawCards[key] = card;
                }
                header.values[key] = parseCardValue(card);
            }
        }

        if (blockOffset > std::numeric_limits<std::size_t>::max() - kFitsBlockSize) {
            header.errorCode = "ImageHeaderInvalid";
            header.message = "FITS header exceeds supported file offsets";
            return header;
        }
        blockOffset += kFitsBlockSize;
    }
}

std::optional<std::int64_t> integerValue(const FitsHeader& header, const std::string& key) {
    const auto it = header.values.find(key);
    if (it == header.values.end()) {
        return std::nullopt;
    }
    try {
        std::size_t parsed = 0;
        const auto value = std::stoll(it->second, &parsed);
        if (parsed != it->second.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> floatingValue(const FitsHeader& header, const std::string& key, double fallback) {
    const auto it = header.values.find(key);
    if (it == header.values.end()) {
        return fallback;
    }
    try {
        std::size_t parsed = 0;
        const auto value = std::stod(it->second, &parsed);
        if (parsed != it->second.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

InspectResult inspectError(std::string code, std::string message, const std::filesystem::path& path) {
    InspectResult result;
    result.ok = false;
    result.metadata.path = path.string();
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

ImageReadResult readError(std::string code, std::string message) {
    ImageReadResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

ImageWriteResult writeError(std::string code, std::string message) {
    ImageWriteResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

std::uint16_t bitsPerChannelForBitpix(int bitpix) {
    switch (bitpix) {
    case 8:
        return 8;
    case 16:
        return 16;
    case 32:
    case -32:
        return 32;
    case -64:
        return 64;
    default:
        return 0;
    }
}

std::size_t bytesPerSample(int bitpix) {
    return static_cast<std::size_t>(std::abs(bitpix) / 8);
}

struct FitsImageDescription {
    int bitpix = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t channels = 0;
};

std::optional<FitsImageDescription> imageDescription(const FitsHeader& header) {
    const auto bitpixValue = integerValue(header, "BITPIX");
    const auto naxis = integerValue(header, "NAXIS");
    const auto width = integerValue(header, "NAXIS1");
    const auto height = integerValue(header, "NAXIS2");
    if (!bitpixValue.has_value() || !naxis.has_value() || !width.has_value() || !height.has_value() ||
        (*naxis != 2 && *naxis != 3) || *width <= 0 || *height <= 0 ||
        *width > std::numeric_limits<std::uint32_t>::max() ||
        *height > std::numeric_limits<std::uint32_t>::max() ||
        *bitpixValue < std::numeric_limits<int>::min() || *bitpixValue > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    std::int64_t channels = 1;
    if (*naxis == 3) {
        const auto depth = integerValue(header, "NAXIS3");
        if (!depth.has_value()) {
            return std::nullopt;
        }
        channels = *depth;
    }
    const int bitpix = static_cast<int>(*bitpixValue);
    if (channels <= 0 || channels > 4 || bitsPerChannelForBitpix(bitpix) == 0) {
        return std::nullopt;
    }

    return FitsImageDescription{
        .bitpix = bitpix,
        .width = static_cast<std::uint32_t>(*width),
        .height = static_cast<std::uint32_t>(*height),
        .channels = static_cast<std::uint16_t>(channels),
    };
}

bool checkedMultiply(std::size_t lhs, std::size_t rhs, std::size_t& result) {
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

std::int16_t readSigned16(const unsigned char* data) {
    const auto value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
    return static_cast<std::int16_t>(value);
}

std::int32_t readSigned32(const unsigned char* data) {
    const auto value = (static_cast<std::uint32_t>(data[0]) << 24U) |
                       (static_cast<std::uint32_t>(data[1]) << 16U) |
                       (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
    return static_cast<std::int32_t>(value);
}

float readFloat32(const unsigned char* data) {
    const std::array<unsigned char, 4> bytes = {data[3], data[2], data[1], data[0]};
    float value = 0.0F;
    static_assert(sizeof(value) == bytes.size());
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<unsigned char*>(&value));
    return value;
}

double readFloat64(const unsigned char* data) {
    const std::array<unsigned char, 8> bytes = {data[7], data[6], data[5], data[4],
                                                data[3], data[2], data[1], data[0]};
    double value = 0.0;
    static_assert(sizeof(value) == bytes.size());
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<unsigned char*>(&value));
    return value;
}

float readPhysicalSample(const unsigned char* data, int bitpix, double bscale, double bzero) {
    double raw = 0.0;
    switch (bitpix) {
    case 8:
        raw = data[0];
        break;
    case 16:
        raw = readSigned16(data);
        break;
    case 32:
        raw = readSigned32(data);
        break;
    case -32:
        raw = readFloat32(data);
        break;
    case -64:
        raw = readFloat64(data);
        break;
    default:
        break;
    }
    return static_cast<float>(raw * bscale + bzero);
}

enum class SampleStreamResult {
    Success,
    ReadFailed,
    AllocationFailed,
};

template <typename Consumer>
SampleStreamResult forEachPhysicalSample(std::ifstream& file, std::size_t dataOffset, std::size_t sampleCount,
                                         std::size_t sampleBytes, int bitpix, double bscale, double bzero,
                                         Consumer&& consume) {
    if (dataOffset > static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max())) {
        return SampleStreamResult::ReadFailed;
    }

    file.clear();
    file.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);
    if (!file) {
        return SampleStreamResult::ReadFailed;
    }

    const auto samplesPerChunk = std::max<std::size_t>(1, kFitsStreamChunkBytes / sampleBytes);
    std::vector<unsigned char> buffer;
    try {
        buffer.resize(samplesPerChunk * sampleBytes);
    } catch (const std::bad_alloc&) {
        return SampleStreamResult::AllocationFailed;
    } catch (const std::length_error&) {
        return SampleStreamResult::AllocationFailed;
    }

    std::size_t sample = 0;
    while (sample < sampleCount) {
        const auto samplesThisChunk = std::min(samplesPerChunk, sampleCount - sample);
        const auto bytesThisChunk = samplesThisChunk * sampleBytes;
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytesThisChunk));
        if (file.gcount() != static_cast<std::streamsize>(bytesThisChunk)) {
            return SampleStreamResult::ReadFailed;
        }
        for (std::size_t localSample = 0; localSample < samplesThisChunk; ++localSample) {
            const auto* bytes = buffer.data() + localSample * sampleBytes;
            consume(sample + localSample, readPhysicalSample(bytes, bitpix, bscale, bzero));
        }
        sample += samplesThisChunk;
    }
    return SampleStreamResult::Success;
}

std::string makeCard(const std::string& key, const std::string& value) {
    std::ostringstream stream;
    stream << std::left << std::setw(8) << key << "= " << std::right << std::setw(20) << value;
    auto card = stream.str();
    if (card.size() > kFitsCardSize) {
        card.resize(kFitsCardSize);
    }
    card.resize(kFitsCardSize, ' ');
    return card;
}

std::string makeEndCard() {
    std::string card = "END";
    card.resize(kFitsCardSize, ' ');
    return card;
}

void appendCard(std::vector<unsigned char>& header, const std::string& card) {
    header.insert(header.end(), card.begin(), card.end());
}

float srgbToLinear(float value) {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    if (clamped <= 0.04045F) {
        return clamped / 12.92F;
    }
    return std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

float sampleForFits(const ImageBuffer& image, std::uint16_t channel, std::size_t offset) {
    const float value = image.pixels[offset];
    const bool isAlphaChannel = image.channels == 4 && channel == 3;
    if (isAlphaChannel) {
        return std::clamp(value, 0.0F, 1.0F);
    }
    return image.colorEncoding == ColorEncoding::SRGB ? srgbToLinear(value) : value;
}

struct SampleRange {
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    bool hasFiniteValue = false;
};

void includeInRange(SampleRange& range, float value) {
    if (!std::isfinite(value)) {
        return;
    }
    range.minimum = std::min(range.minimum, value);
    range.maximum = std::max(range.maximum, value);
    range.hasFiniteValue = true;
}

float normalizeForDisplay(float value, const SampleRange& range) {
    if (!std::isfinite(value) || !range.hasFiniteValue) {
        return 0.0F;
    }
    if (range.minimum >= 0.0F && range.maximum <= 1.0F) {
        return std::clamp(value, 0.0F, 1.0F);
    }
    const float span = range.maximum - range.minimum;
    if (!(span > 0.0F) || !std::isfinite(span)) {
        return std::clamp(value, 0.0F, 1.0F);
    }
    return std::clamp((value - range.minimum) / span, 0.0F, 1.0F);
}

} // namespace

DevelopableFitsImage readDevelopableFits(const std::filesystem::path& path, bool readWcs) {
    std::ifstream file(path, std::ios::binary);
    const auto header = parseHeader(file);
    if (!header.ok) throw std::runtime_error(header.message);
    const auto description = imageDescription(header);
    if (!description || (description->channels != 1 && description->channels != 3 && description->channels != 4) || header.values.contains("BAYERPAT"))
        throw std::invalid_argument("Select a linear mono/RGB master, not a CFA exposure");
    const auto profile = header.values.find("PSCOLOR");
    if (profile != header.values.end() && profile->second != "Linear" && profile->second != "LINEAR-SRGB" && profile->second != "SRGB-DISPLAY")
        throw std::invalid_argument("Unsupported FITS master color encoding; expected linear or SRGB-DISPLAY");
    DevelopableFitsImage result;
    for (const auto* key : {"CTYPE1", "CTYPE2", "CRPIX1", "CRPIX2", "CRVAL1", "CRVAL2",
                            "CD1_1", "CD1_2", "CD2_1", "CD2_2", "CDELT1", "CDELT2",
                            "PC1_1", "PC1_2", "PC2_1", "PC2_2", "A_ORDER", "B_ORDER", "WCSAXES"})
        if (readWcs && header.values.contains(key)) { result.sky = readSolvedPanel(path); break; }
    FitsDecodeOptions options; options.mode = FitsDecodeMode::Scientific; options.maskNonFinitePixels = true;
    auto decoded = FitsCodec().read(path, options);
    if (!decoded.ok) throw std::runtime_error(decoded.message);
    result.image = std::move(decoded.image);
    return result;
}

LinearScienceImage readLinearScienceImage(const std::filesystem::path& path) {
    auto result = readDevelopableFits(path);
    if (result.image.colorEncoding != ColorEncoding::Linear)
        throw std::invalid_argument("Select a linear FITS master");
    return result;
}

LinearMonoMaster readLinearMonoMaster(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const auto header = parseHeader(file);
    if (!header.ok) throw std::runtime_error(header.message);
    const auto description = imageDescription(header);
    if (!description || description->channels != 1 || header.values.contains("BAYERPAT"))
        throw std::invalid_argument("Select a mono master, not an RGB image or CFA exposure");
    const auto profile = header.values.find("PSCOLOR");
    if (profile != header.values.end() && profile->second != "Linear" && profile->second != "LINEAR-SRGB")
        throw std::invalid_argument("Channel composition requires linear intensities");
    LinearMonoMaster result;
    for (const auto* key : {"CTYPE1", "CTYPE2", "CRPIX1", "CRPIX2", "CRVAL1", "CRVAL2",
                            "CD1_1", "CD1_2", "CD2_1", "CD2_2", "CDELT1", "CDELT2",
                            "PC1_1", "PC1_2", "PC2_1", "PC2_2", "A_ORDER", "B_ORDER", "WCSAXES"})
        if (header.values.contains(key)) { result.sky = readSolvedPanel(path); break; }
    FitsDecodeOptions options; options.mode = FitsDecodeMode::Scientific;
    auto decoded = FitsCodec().read(path, options);
    if (!decoded.ok) throw std::runtime_error(decoded.message);
    auto& mono = result.image;
    mono.width = decoded.image.width; mono.height = decoded.image.height;
    mono.channels = 1; mono.format = PixelFormat::Float32Gray;
    mono.pixels.resize(mono.pixelCount());
    for (std::size_t i = 0; i < mono.pixelCount(); ++i) mono.pixels[i] = decoded.image.pixels[i * decoded.image.channels];
    return result;
}

static SolvedPanel readPanelWcs(const std::filesystem::path& path, bool astrometryICRS) {
    std::ifstream file(path, std::ios::binary);
    const auto header = parseHeader(file);
    if (!header.ok)
        throw std::runtime_error(header.message);
    const auto& h = header.values;
    auto value = [&](const std::string& key) { return h.contains(key) ? h.at(key) : std::string(); };
    auto number = [&](const std::string& key) {
        auto v = value(key);
        std::replace(v.begin(), v.end(), 'D', 'E');
        std::size_t end = 0;
        try {
            const auto n = std::stod(v, &end);
            if (end == v.size() && std::isfinite(n))
                return n;
        } catch (const std::exception&) {
        }
        throw std::runtime_error("Missing or invalid WCS keyword: " + key);
    };
    const bool sip = value("CTYPE1") == "RA---TAN-SIP" && value("CTYPE2") == "DEC--TAN-SIP";
    if (!sip && !(value("CTYPE1") == "RA---TAN" && value("CTYPE2") == "DEC--TAN"))
        throw std::runtime_error("A solved RA/DEC TAN or TAN-SIP FITS panel is required");
    if ((!value("RADESYS").empty() && value("RADESYS") != "ICRS") || h.contains("RADECSYS") || (h.contains("EQUINOX") && (!astrometryICRS || number("EQUINOX") != 2000)) ||
        (h.contains("LONPOLE") && number("LONPOLE") != 180) ||
        (h.contains("LATPOLE") && std::abs(number("LATPOLE")) > 90))
        throw std::runtime_error("Only ICRS with standard celestial pole orientation is supported");
    for (auto key : {"CUNIT1", "CUNIT2"})
        if (h.contains(key) && value(key) != "deg")
            throw std::runtime_error("Expected WCS units in degrees");
    for (const auto& [key, v] : h)
        if (key.starts_with("PV1_") || key.starts_with("PV2_") || key.starts_with("CPDIS") || key.starts_with("D2IM") ||
            key.starts_with("CROTA") || (!sip && (key == "A_ORDER" || key == "B_ORDER")))
            throw std::runtime_error("Unsupported WCS distortion or rotation: " + key);
    // A color cube may have an independent channel axis, but a coupled
    // celestial/spectral matrix cannot be represented by one 2D projection.
    for (auto key : {"CD1_3", "CD2_3", "CD3_1", "CD3_2", "PC1_3", "PC2_3", "PC3_1", "PC3_2"})
        if (h.contains(key) && number(key) != 0)
            throw std::runtime_error("Coupled cube WCS is unsupported");
    SolvedPanel panel{};
    auto& g = panel.grid;
    g.x = number("CRPIX1");
    g.y = number("CRPIX2");
    g.ra = number("CRVAL1");
    g.dec = number("CRVAL2");
    if (std::abs(g.dec) >= 90)
        throw std::runtime_error("Unsupported tangent-point declination");
    const bool cd = h.contains("CD1_1") || h.contains("CD1_2") || h.contains("CD2_1") || h.contains("CD2_2");
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            auto suffix = std::to_string(i + 1) + "_" + std::to_string(j + 1);
            if (cd && h.contains("PC" + suffix))
                throw std::runtime_error("Conflicting CD and PC matrices");
            g.cd[2 * i + j] = cd ? (h.contains("CD" + suffix) ? number("CD" + suffix) : 0)
                                 : number("CDELT" + std::to_string(i + 1)) *
                                       (h.contains("PC" + suffix) ? number("PC" + suffix) : double(i == j));
        }
    if (std::abs(g.cd[0] * g.cd[3] - g.cd[1] * g.cd[2]) < 1e-18)
        throw std::runtime_error("Singular WCS pixel scale");
    if (sip) {
        const auto a = number("A_ORDER"), b = number("B_ORDER");
        if (a < 2 || b < 2 || a > 5 || b > 5 || a != std::floor(a) || b != std::floor(b))
            throw std::runtime_error("Supported SIP polynomial orders are 2 through 5");
        for (unsigned i = 0; i <= std::max(a, b); ++i)
            for (unsigned j = 0; i + j <= std::max(a, b); ++j) {
                auto suffix = "_" + std::to_string(i) + "_" + std::to_string(j);
                auto ca = h.contains("A" + suffix) ? number("A" + suffix) : 0;
                auto cb = h.contains("B" + suffix) ? number("B" + suffix) : 0;
                if ((ca && i + j > a) || (cb && i + j > b))
                    throw std::runtime_error("Invalid SIP coefficient degree");
                if (ca || cb)
                    panel.distortion.push_back({i, j, ca, cb});
            }
    }
    return panel;
}

SolvedPanel readSolvedPanel(const std::filesystem::path& path) { return readPanelWcs(path, false); }
SolvedPanel readAstrometryICRSPanel(const std::filesystem::path& path) { return readPanelWcs(path, true); }


bool isFitsPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".fits" || extension == ".fit" || extension == ".fts";
}

bool FitsSensorIdentity::sameAcquisition(const FitsSensorIdentity& b) const {
    return ok && b.ok && width == b.width && height == b.height && camera == b.camera &&
        bayer == b.bayer && xOffset == b.xOffset && yOffset == b.yOffset && exposure == b.exposure && gain == b.gain;
}

CalibrationAcquisition inspectCalibrationAcquisition(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    const auto header = parseHeader(stream, true);
    if (!header.ok) throw std::invalid_argument(header.message);
    const auto desc = imageDescription(header);
    if (!desc || !header.values.contains("SIMPLE") || header.values.at("SIMPLE") != "T")
        throw std::invalid_argument("Calibration metadata requires a supported primary FITS image");
    for (const auto* key : {"SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "NAXIS2", "NAXIS3"})
        if (header.rawCards.contains(key) && trim(header.rawCards.at(key).substr(10)).starts_with('\''))
            throw std::invalid_argument("Structural FITS values cannot be strings");
    std::size_t count = 0, samples = 0, bytes = 0;
    const auto size = std::filesystem::file_size(path);
    if (!checkedMultiply(desc->width, desc->height, count) || !checkedMultiply(count, desc->channels, samples) ||
        !checkedMultiply(samples, bytesPerSample(desc->bitpix), bytes) || header.dataOffset > size ||
        size - header.dataOffset < bytes)
        throw std::invalid_argument("Calibration image data are truncated or too large");
    CalibrationAcquisition r;
    r.width = desc->width; r.height = desc->height; r.channels = desc->channels; r.bitpix = desc->bitpix;
    auto issue = [&](const std::string& field, const std::string& code, const std::string& message) {
        r.issues.push_back({field, code, message});
    };
    for (const auto& key : {"SIMPLE", "BITPIX", "NAXIS", "NAXIS1", "NAXIS2", "NAXIS3", "BSCALE", "BZERO"})
        if (header.duplicateKeys.contains(key)) issue(key, "duplicate", "Repeated structural FITS keyword");
    auto text = [&](const std::string& field, const std::string& key, bool allowUnquoted = false) {
        if (!header.values.contains(key)) return;
        r.keys[field].push_back(key);
        if (header.duplicateKeys.contains(key)) { issue(field, "duplicate", key + " is repeated"); return; }
        const auto raw = trim(header.rawCards.at(key).substr(10));
        std::string value;
        if (raw.empty()) return;
        if (raw.front() == '\'') {
            bool closed = false;
            for (std::size_t i = 1; i < raw.size(); ++i) {
                if (raw[i] != '\'') { value += raw[i]; continue; }
                if (i + 1 < raw.size() && raw[i + 1] == '\'') { value += '\''; ++i; continue; }
                const auto rest = trim(raw.substr(i + 1));
                closed = rest.empty() || rest.front() == '/'; break;
            }
            if (!closed) { issue(field, "invalid", key + " is not a complete FITS string"); return; }
        } else if (allowUnquoted && integerValue(header, key)) value = header.values.at(key);
        else { issue(field, "invalid", key + " requires a FITS string"); return; }
        value = trim(value);
        if (!value.empty()) r.text[field] = value;
    };
    auto number = [&](const std::string& field, const std::vector<std::string>& keys,
                      double low, double high, bool integer = false) {
        std::optional<double> value;
        bool invalid = false;
        for (const auto& key : keys) {
            if (!header.values.contains(key)) continue;
            r.keys[field].push_back(key);
            try {
                if (header.duplicateKeys.contains(key)) throw std::invalid_argument("repeated keyword");
                auto input = header.values.at(key);
                if (trim(header.rawCards.at(key).substr(10)).starts_with('\''))
                    throw std::invalid_argument("number stored as a string");
                std::replace(input.begin(), input.end(), 'D', 'E');
                std::replace(input.begin(), input.end(), 'd', 'e');
                if (input.find_first_not_of("0123456789+-.eE") != std::string::npos)
                    throw std::invalid_argument("unsupported numeric syntax");
                std::size_t consumed = 0; const auto n = std::stod(input, &consumed);
                if (consumed != input.size() || !std::isfinite(n) || n < low || n > high ||
                    (integer && n != std::floor(n))) throw std::invalid_argument("invalid range or numeric syntax");
                if (value && *value != n) {
                    issue(field, "conflict", "Acquisition aliases disagree"); invalid = true;
                }
                value = n;
            } catch (const std::exception& e) { issue(field, "invalid", key + ": " + e.what()); invalid = true; }
        }
        if (value && !invalid) r.numeric[field] = *value;
    };
    text("camera", "INSTRUME"); text("cameraSerial", "CAM-SER"); text("filter", "FILTER");
    text("opticalTrain", "PSOPTICS"); text("readoutMode", "READMODE", true);
    text("object", "OBJECT"); text("observedAt", "DATE-OBS"); text("timeSystem", "TIMESYS"); text("frameType", "IMAGETYP");
    text("units", "BUNIT"); text("domain", "PSCOLOR"); text("declaredLayout", "PSLAYOUT");
    text("bayer", "BAYERPAT"); text("temperatureUnit", "TEMPUNIT");
    text("biasState", "PSBIAS"); text("normalization", "PSNORM"); text("calibration", "PSCALIB");
    if (r.text.contains("biasState") && !std::set<std::string>{"included","removed"}.contains(r.text.at("biasState")))
        issue("biasState", "invalid", "PSBIAS must be included or removed");
    number("exposure", {"EXPTIME", "EXPOSURE"}, 0, 1e8);
    number("gain", {"GAIN"}, 0, 1e9); number("iso", {"ISOSPEED"}, 1, 1e9, true);
    number("offset", {"OFFSET"}, 0, 1e9); number("electronsPerADU", {"EGAIN"}, 1e-12, 1e9);
    number("sensorTemperature", {"CCD-TEMP"}, -273.15, 200);
    number("setTemperature", {"SET-TEMP"}, -273.15, 200);
    number("sensorTemperatureMin", {"PSTMIN"}, -273.15, 200);
    number("sensorTemperatureMax", {"PSTMAX"}, -273.15, 200);
    if (header.values.contains("PSTMIN") || header.values.contains("PSTMAX")) {
        if (!r.numeric.contains("sensorTemperatureMin") || !r.numeric.contains("sensorTemperatureMax") ||
            r.numeric.at("sensorTemperatureMin") > r.numeric.at("sensorTemperatureMax") ||
            (r.numeric.contains("sensorTemperature") &&
             (r.numeric.at("sensorTemperature") < r.numeric.at("sensorTemperatureMin") ||
              r.numeric.at("sensorTemperature") > r.numeric.at("sensorTemperatureMax"))))
            issue("sensorTemperature", "invalid", "Incomplete or inconsistent master temperature range");
    }
    number("binX", {"XBINNING"}, 1, 65535, true); number("binY", {"YBINNING"}, 1, 65535, true);
    number("originX", {"XORGSUBF"}, 0, 1e9, true); number("originY", {"YORGSUBF"}, 0, 1e9, true);
    number("focalLength", {"FOCALLEN"}, 1e-9, 1e9);
    number("scale", {"BSCALE"}, 1e-300, 1e300); number("zero", {"BZERO"}, -1e300, 1e300);
    if (!header.values.contains("BSCALE")) r.numeric["scale"] = 1;
    if (!header.values.contains("BZERO")) r.numeric["zero"] = 0;
    auto upper = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); }); return s;
    };
    if (r.text.contains("temperatureUnit") && !std::set<std::string>{"C", "CELSIUS", "DEGC"}.contains(upper(r.text.at("temperatureUnit")))) {
        issue("sensorTemperature", "unsupported", "CCD-TEMP is supported only in degrees Celsius");
        r.numeric.erase("sensorTemperature");
    }
    if (r.text.contains("units") && upper(r.text.at("units")) == "ADU") r.text["units"] = "ADU";
    if (r.text.contains("domain")) {
        const auto value = upper(r.text.at("domain"));
        if (value == "LINEAR" || value == "LINEAR-SRGB") r.text["domain"] = "linear";
        else issue("domain", "unsupported", "Calibration requires linear sensor samples");
    }
    const auto layout = r.text.contains("declaredLayout") ? upper(r.text.at("declaredLayout")) : "";
    if (r.text.contains("bayer")) {
        const auto bayer = upper(r.text.at("bayer"));
        if (!std::set<std::string>{"RGGB", "BGGR", "GRBG", "GBRG"}.contains(bayer) || r.channels != 1 ||
            (!layout.empty() && layout != "CFA")) issue("sampling", "invalid", "CFA pattern and declared layout disagree or are unsupported");
        else {
            std::string effective;
            auto phase = [&](const char* key) -> unsigned {
                if (!header.values.contains(key)) return 0;
                const auto n = integerValue(header, key);
                if (!n || header.duplicateKeys.contains(key) || trim(header.rawCards.at(key).substr(10)).starts_with('\'')) {
                    issue("sampling", "invalid", std::string("Invalid CFA phase: ") + key); return 0;
                }
                return static_cast<unsigned>(*n) & 1;
            };
            const auto dx = phase("XBAYROFF"), dy = phase("YBAYROFF");
            for (unsigned i = 0; i < 4; ++i) effective += bayer[i ^ dx ^ (dy << 1)];
            r.text["sampling"] = "CFA:" + effective;
            r.keys["sampling"] = {"BAYERPAT"};
            for (const auto* key : {"XBAYROFF", "YBAYROFF"}) if (header.values.contains(key)) r.keys["sampling"].push_back(key);
        }
    } else if (layout == "MONO" && r.channels == 1) {
        r.text["sampling"] = "mono"; r.keys["sampling"] = {"PSLAYOUT"};
    } else if (!layout.empty()) issue("sampling", "unsupported", "Explicit MONO or complete CFA layout is required");
    return r;
}

CfaSensorInfo inspectCfaSensor(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    const auto header = parseHeader(stream);
    if (!header.ok) throw std::runtime_error(header.message);
    const auto desc = imageDescription(header);
    if (!desc || desc->channels != 1 || desc->width < 2 || desc->height < 2)
        throw std::invalid_argument("Calibration requires an un-debayered CFA FITS plane");
    std::size_t count = 0, bytes = 0;
    const auto size = std::filesystem::file_size(path);
    if (!checkedMultiply(desc->width, desc->height, count) || !checkedMultiply(count, bytesPerSample(desc->bitpix), bytes) ||
        header.dataOffset > size || size - header.dataOffset < bytes)
        throw std::invalid_argument("Calibration CFA data section is truncated or too large");
    const auto tag = header.values.find("BAYERPAT");
    std::string pattern = tag == header.values.end() ? "" : tag->second;
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), [](unsigned char c) { return std::toupper(c); });
    if (pattern != "RGGB" && pattern != "BGGR" && pattern != "GBRG" && pattern != "GRBG")
        throw std::invalid_argument("Calibration master requires an explicit supported BAYERPAT");
    const auto profile = header.values.find("PSCOLOR");
    if (profile != header.values.end() && profile->second != "Linear" && profile->second != "LINEAR-SRGB")
        throw std::invalid_argument("Calibration requires linear sensor data");
    if (desc->bitpix > 0 && header.values.contains("BLANK"))
        throw std::invalid_argument("Integer CFA with BLANK requires an explicit validity mask");
    const auto phase = [&](const char* key) -> unsigned {
        if (!header.values.contains(key)) return 0;
        const auto v = integerValue(header, key);
        if (!v) throw std::invalid_argument("Invalid CFA offset");
        return static_cast<unsigned>(*v) & 1U;
    };
    CfaSensorInfo result;
    result.width = desc->width; result.height = desc->height;
    for (unsigned p = 0; p < 4; ++p) result.colors[p] = pattern[p ^ phase("XBAYROFF") ^ (phase("YBAYROFF") << 1)];
    if (header.values.contains("INSTRUME")) result.camera = header.values.at("INSTRUME");
    const auto number = [&](const char* key) -> std::optional<double> {
        if (!header.values.contains(key)) return std::nullopt;
        const auto value = floatingValue(header, key, 0);
        if (!value || !std::isfinite(*value) || *value < 0) throw std::invalid_argument(std::string("Invalid CFA acquisition field: ") + key);
        return value;
    };
    result.exposure = number(header.values.contains("EXPOSURE") ? "EXPOSURE" : "EXPTIME");
    result.gain = number("GAIN"); result.binX = number("XBINNING"); result.binY = number("YBINNING");
    return result;
}

ImageBuffer readCfaSensor(const std::filesystem::path& path) {
    const auto info = inspectCfaSensor(path);
    std::ifstream stream(path, std::ios::binary);
    const auto header = parseHeader(stream);
    const auto desc = imageDescription(header);
    if (!header.ok || !desc || desc->width != info.width || desc->height != info.height || desc->channels != 1)
        throw std::invalid_argument("Calibration FITS header changed during reading");
    const auto scale = floatingValue(header, "BSCALE", 1), zero = floatingValue(header, "BZERO", 0);
    if (!scale || !zero) throw std::invalid_argument("Invalid FITS physical scale");
    const auto size = std::filesystem::file_size(path);
    std::size_t count = 0, bytes = 0;
    if (!checkedMultiply(info.width, info.height, count) || !checkedMultiply(count, bytesPerSample(desc->bitpix), bytes) ||
        header.dataOffset > size || size - header.dataOffset < bytes)
        throw std::invalid_argument("Calibration FITS data section is truncated or too large");
    ImageBuffer result;
    result.width = info.width; result.height = info.height;
    result.channels = 1; result.format = PixelFormat::Float32Gray; result.pixels.resize(count);
    const auto status = forEachPhysicalSample(stream, header.dataOffset, count, bytesPerSample(desc->bitpix), desc->bitpix,
        *scale, *zero, [&](std::size_t p, float value) { result.pixels[p] = value; });
    if (status != SampleStreamResult::Success) throw std::runtime_error("Could not read calibration CFA samples");
    return result;
}

struct SensorFitsReader::Impl {
    CalibrationAcquisition acquisition;
    std::filesystem::path path;
    std::ifstream stream;
    std::size_t offset = 0, count = 0, stride = 0;
    double scale = 1, zero = 0;
    std::optional<long long> blank;
    std::vector<unsigned char> bytes;
};
SensorFitsReader::SensorFitsReader(const std::filesystem::path& path): impl_(std::make_unique<Impl>()) {
    auto& r = *impl_;
    r.acquisition = inspectCalibrationAcquisition(path);
    const auto& a = r.acquisition;
    if (a.channels != 1 || !a.issues.empty() || !a.text.contains("sampling") ||
        !a.text.contains("domain") || a.text.at("domain") != "linear")
        throw std::invalid_argument("Sensor frames require valid explicit linear single-plane acquisition metadata");
    r.stream.open(path,std::ios::binary);
    const auto h = parseHeader(r.stream,true);
    if (!h.ok) throw std::runtime_error(h.message);
    r.offset = h.dataOffset; r.count = std::size_t(a.width)*a.height; r.stride = bytesPerSample(a.bitpix);
    r.scale = a.numeric.at("scale"); r.zero = a.numeric.at("zero");
    if (h.values.contains("BLANK")) {
        r.blank = integerValue(h,"BLANK");
        const auto low = a.bitpix == 8 ? 0LL : a.bitpix == 16 ? -32768LL : -2147483648LL;
        const auto high = a.bitpix == 8 ? 255LL : a.bitpix == 16 ? 32767LL : 2147483647LL;
        if (a.bitpix < 0 || !r.blank || *r.blank < low || *r.blank > high || h.duplicateKeys.contains("BLANK") ||
            trim(h.rawCards.at("BLANK").substr(10)).starts_with('\''))
            throw std::invalid_argument("Invalid integer sensor BLANK keyword");
    }
    r.path=path; r.stream.close(); r.bytes.resize(65536);
}
SensorFitsReader::~SensorFitsReader() = default;
SensorFitsReader::SensorFitsReader(SensorFitsReader&&) noexcept = default;
SensorFitsReader& SensorFitsReader::operator=(SensorFitsReader&&) noexcept = default;
const CalibrationAcquisition& SensorFitsReader::metadata() const { return impl_->acquisition; }
void SensorFitsReader::read(std::size_t start, std::span<float> samples) {
    auto& r = *impl_;
    if (start > r.count || samples.size() > r.count-start) throw std::invalid_argument("Sensor read range exceeds image");
    r.stream.close(); r.stream.clear(); r.stream.open(r.path,std::ios::binary);
    r.stream.seekg(static_cast<std::streamoff>(r.offset + start*r.stride));
    for (std::size_t i=0; i<samples.size();) {
        const auto n=std::min(samples.size()-i,r.bytes.size()/r.stride);
        r.stream.read(reinterpret_cast<char*>(r.bytes.data()),std::streamsize(n*r.stride));
        if (!r.stream) throw std::runtime_error("Sensor file changed or cannot be read");
        for (std::size_t j=0;j<n;++j) {
            const auto* p=r.bytes.data()+j*r.stride;
            const double raw=r.acquisition.bitpix==8 ? p[0] : r.acquisition.bitpix==16 ? readSigned16(p) :
                r.acquisition.bitpix==32 ? readSigned32(p) : r.acquisition.bitpix==-32 ? readFloat32(p) : readFloat64(p);
            if (!std::isfinite(raw) || (r.blank && raw == double(*r.blank))) {
                samples[i+j]=std::numeric_limits<float>::quiet_NaN(); continue;
            }
            const double physical=raw*r.scale+r.zero;
            if (!std::isfinite(physical) || std::abs(physical)>std::numeric_limits<float>::max())
                throw std::invalid_argument("Sensor physical sample exceeds Float32 range");
            samples[i+j]=static_cast<float>(physical);
        }
        i+=n;
    }
    r.stream.close();
}

DualBandPlanes readDualBandPlanes(const std::filesystem::path& path, const CfaCalibration* calibration, DualBandSampling sampling) {
    if (sampling != DualBandSampling::CellAverage && sampling != DualBandSampling::RedGrid)
        throw std::invalid_argument("Unsupported dual-band sampling mode");
    if (calibration) calibration->validate(inspectCfaSensor(path));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open CFA FITS input");
    const auto header = parseHeader(stream);
    if (!header.ok) throw std::runtime_error(header.message);
    const auto desc = imageDescription(header);
    if (!desc || desc->channels != 1 || desc->width < 2 || desc->height < 2)
        throw std::runtime_error("Dual-band extraction requires a single CFA plane of at least 2x2 pixels");
    const auto tag = header.values.find("BAYERPAT");
    std::string pattern = tag == header.values.end() ? "" : tag->second;
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), [](unsigned char c) { return std::toupper(c); });
    if (pattern != "RGGB" && pattern != "BGGR" && pattern != "GBRG" && pattern != "GRBG")
        throw std::runtime_error("Missing or unsupported Bayer pattern");
    const auto phase = [&](const char* key) -> unsigned {
        if (!header.values.contains(key)) return 0;
        const auto value = integerValue(header, key);
        if (!value) throw std::runtime_error("Invalid CFA offset");
        return static_cast<unsigned>(*value) & 1U;
    };
    const unsigned red = unsigned(pattern.find('R')) ^ phase("XBAYROFF") ^ (phase("YBAYROFF") << 1);
    const auto scale = floatingValue(header, "BSCALE", 1), zero = floatingValue(header, "BZERO", 0);
    if (!scale || !zero) throw std::runtime_error("Invalid FITS physical scale");
    if (header.values.contains("BLANK") && desc->bitpix > 0)
        throw std::runtime_error("Integer FITS with BLANK requires a validity mask before extraction");
    const auto profile = header.values.find("PSCOLOR");
    if (profile != header.values.end() && profile->second != "Linear" && profile->second != "LINEAR-SRGB")
        throw std::runtime_error("Dual-band extraction requires linear sensor data");
    const auto fileSize = std::filesystem::file_size(path);
    std::size_t samples = 0, bytes = 0;
    if (!checkedMultiply(desc->width, desc->height, samples) ||
        !checkedMultiply(samples, bytesPerSample(desc->bitpix), bytes) ||
        header.dataOffset > fileSize || fileSize - header.dataOffset < bytes)
        throw std::runtime_error("FITS data section is truncated or too large");
    DualBandPlanes result;
    const auto width = desc->width / 2, height = desc->height / 2;
    for (auto* image : {&result.hydrogen, &result.oxygen}) {
        image->width = width;
        image->height = height;
        image->channels = 1;
        image->format = PixelFormat::Float32Gray;
        image->pixels.resize(image->pixelCount());
    }
    if (sampling == DualBandSampling::RedGrid) {
        // Three rows suffice to interpolate G1, G2 and B symmetrically at each
        // red site. Each green contributes half its two neighbors; blue uses
        // four diagonal neighbors. Their equal mean preserves the original
        // 2:1 green/blue DC response, with zero spatial first moment.
        const auto missing = std::numeric_limits<float>::quiet_NaN();
        std::fill(result.oxygen.pixels.begin(), result.oxygen.pixels.end(), missing);
        std::vector<float> rows(std::size_t(desc->width) * 3, missing);
        const unsigned rx = red & 1U, ry = red >> 1;
        const auto status = forEachPhysicalSample(stream, header.dataOffset, samples, bytesPerSample(desc->bitpix),
            desc->bitpix, *scale, *zero, [&](std::size_t pixel, float value) {
                const auto x = pixel % desc->width, y = pixel / desc->width;
                if (calibration) value = calibration->apply(pixel, value);
                if (!std::isfinite(value)) value = missing;
                rows[(y % 3) * desc->width + x] = value;
                if (x % 2 == rx && y % 2 == ry && x / 2 < width && y / 2 < height)
                    result.hydrogen.pixels[y / 2 * width + x / 2] = value;
                if (x + 1 != desc->width || y < 2) return;
                const auto cy = y - 1;
                if (cy % 2 != ry || cy / 2 >= height) return;
                const float* above = rows.data() + ((cy - 1) % 3) * desc->width;
                const float* center = rows.data() + (cy % 3) * desc->width;
                const float* below = rows.data() + ((cy + 1) % 3) * desc->width;
                for (std::size_t cx = rx; cx / 2 < width; cx += 2) {
                    if (cx == 0 || cx + 1 >= desc->width) continue;
                    const double green = double(center[cx - 1]) + center[cx + 1] + above[cx] + below[cx];
                    const double blue = double(above[cx - 1]) + above[cx + 1] + below[cx - 1] + below[cx + 1];
                    result.oxygen.pixels[cy / 2 * width + cx / 2] = static_cast<float>((2 * green + blue) / 12);
                }
            });
        if (status != SampleStreamResult::Success) throw std::runtime_error("Could not read CFA samples");
        return result;
    }
    // Keep only one row of double accumulators; signed faint signals and
    // finite values near Float32.max must survive the three-sample mean.
    std::vector<double> sums(width);
    const auto status = forEachPhysicalSample(stream, header.dataOffset, samples, bytesPerSample(desc->bitpix),
        desc->bitpix, *scale, *zero, [&](std::size_t pixel, float value) {
            const auto x = pixel % desc->width, y = pixel / desc->width;
            if (x / 2 >= width || y / 2 >= height) return;
            const auto cell = y / 2 * width + x / 2;
            const unsigned colorPhase = unsigned(2 * (y % 2) + x % 2);
            if (calibration) value = calibration->apply(pixel, value);
            if (!std::isfinite(value)) value = std::numeric_limits<float>::quiet_NaN();
            if (colorPhase == red) result.hydrogen.pixels[cell] = value;
            else sums[x / 2] += value;
            if (colorPhase == 3) {
                result.oxygen.pixels[cell] = static_cast<float>(sums[x / 2] / 3.0);
                sums[x / 2] = 0;
            }
        });
    if (status != SampleStreamResult::Success) throw std::runtime_error("Could not read CFA samples");
    return result;
}

FitsSensorIdentity FitsCodec::inspectSensor(const std::filesystem::path& path) const {
    FitsSensorIdentity result;
    const auto fail = [&](const char* message) {
        result.errorCode = "SensorIdentityInvalid"; result.message = message; return result;
    };
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail("Cannot open sensor FITS input");
    const auto header = parseHeader(stream);
    if (!header.ok) return fail("Cannot parse sensor FITS header");
    if (header.values.contains("PSCALIB")) return fail("Sensor pattern estimation requires uncalibrated sensor frames");
    const auto description = imageDescription(header);
    if (!description || description->channels != 1) return fail("Sensor model requires a single CFA plane");
    const auto get = [&](const char* key) {
        const auto it = header.values.find(key); return it == header.values.end() ? std::string{} : it->second;
    };
    result.bayer = get("BAYERPAT");
    std::transform(result.bayer.begin(), result.bayer.end(), result.bayer.begin(),
        [](unsigned char c) { return std::toupper(c); });
    if (result.bayer != "RGGB" && result.bayer != "GRBG" && result.bayer != "GBRG" && result.bayer != "BGGR")
        return fail("Sensor model requires a supported explicit Bayer pattern");
    result.camera = get("INSTRUME");
    const auto telescope = get("TELESCOP");
    if (!telescope.empty()) result.camera += "|" + telescope;
    if (result.camera.empty()) return fail("Sensor model requires a camera identity");
    const auto exposureKey = header.values.contains("EXPOSURE") ? "EXPOSURE" : "EXPTIME";
    if (!header.values.contains(exposureKey) || !header.values.contains("GAIN"))
        return fail("Sensor model requires explicit exposure and gain");
    const auto exposure = floatingValue(header, exposureKey, 0), gain = floatingValue(header, "GAIN", 0);
    if (!exposure || !gain || !std::isfinite(*exposure) || !std::isfinite(*gain) || *exposure <= 0 || *gain < 0)
        return fail("Invalid sensor exposure or gain");
    const auto profile = get("PSCOLOR");
    if (!profile.empty() && profile != "Linear" && profile != "LINEAR-SRGB")
        return fail("Sensor model requires linear acquisition samples");
    for (const auto* key : {"XBAYROFF", "YBAYROFF"}) {
        const auto offset = integerValue(header, key);
        if (header.values.contains(key) && !offset) return fail("Invalid Bayer offset");
        const int parity = offset ? int((*offset % 2 + 2) % 2) : 0;
        if (std::string(key) == "XBAYROFF") result.xOffset = parity; else result.yOffset = parity;
    }
    result.width = description->width; result.height = description->height;
    result.exposure = *exposure; result.gain = *gain; result.ok = true;
    return result;
}

InspectResult FitsCodec::inspect(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path)) {
        return inspectError("InputFileNotFound", "Input file does not exist", path);
    }

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize == 0) {
        return inspectError("InputReadFailed", "Input file could not be read or is empty", path);
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return inspectError("InputReadFailed", "Input file could not be read or is empty", path);
    }
    const auto header = parseHeader(file);
    if (!header.ok) {
        return inspectError(header.errorCode, header.message, path);
    }

    const auto description = imageDescription(header);
    if (!description.has_value()) {
        return inspectError("ImageHeaderInvalid", "FITS header does not describe a supported image HDU", path);
    }

    InspectResult result;
    result.ok = true;
    result.metadata.path = path.string();
    result.metadata.format = ImageFormat::FITS;
    result.metadata.width = description->width;
    result.metadata.height = description->height;
    result.metadata.channels = description->channels;
    result.metadata.bitsPerChannel = bitsPerChannelForBitpix(description->bitpix);
    result.metadata.colorModel = description->channels == 1 ? "Gray" : "RGB";
    const auto colorProfile = header.values.find("PSCOLOR");
    result.metadata.colorProfile = colorProfile == header.values.end() ? "Linear (assumed)" : colorProfile->second;
    return result;
}

ImageReadResult FitsCodec::read(const std::filesystem::path& path) const {
    return read(path, {});
}

ImageReadResult FitsCodec::read(const std::filesystem::path& path, const FitsDecodeOptions& options) const {
    return read(path, options, nullptr);
}

static ImageReadResult readFits(const std::filesystem::path& path, const FitsDecodeOptions& options,
                                const CalibrationOptions* calibration, const ImageBuffer* sensorPattern,
                                detail::GuardReplay* replay) {
    const detail::StageScope timer(detail::Stage::FitsDecode);
    if (sensorPattern && (options.mode != FitsDecodeMode::Scientific ||
        (calibration && (calibration->bias || calibration->dark || calibration->flat))))
        return readError("SensorPatternConflict", "Sensor pattern requires scientific decoding without other calibration masters");
    if (options.mode != FitsDecodeMode::DisplayNormalized && options.mode != FitsDecodeMode::Scientific) {
        return readError("ArgumentInvalid", "FITS decode options are invalid");
    }
    if (!std::filesystem::exists(path)) {
        return readError("InputFileNotFound", "Input file does not exist");
    }

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize == 0) {
        return readError("InputReadFailed", "Input file could not be read or is empty");
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return readError("InputReadFailed", "Input file could not be read or is empty");
    }
    const auto header = parseHeader(file);
    if (!header.ok) {
        return readError(header.errorCode, header.message);
    }

    if (header.values.contains("PSCALIB") &&
        (sensorPattern || (calibration && (calibration->bias || calibration->dark || calibration->flat)))) {
        return readError("AlreadyCalibrated", "This sensor frame is already calibrated; disable calibration masters and sensor pattern correction");
    }

    const auto description = imageDescription(header);
    if (!description.has_value()) {
        return readError("ImageHeaderInvalid", "FITS header does not describe a supported 2D or 3D image HDU");
    }

    std::size_t pixelCount = 0;
    std::size_t sourceSamples = 0;
    std::size_t dataBytes = 0;
    const auto sampleBytes = bytesPerSample(description->bitpix);
    if (!checkedMultiply(description->width, description->height, pixelCount) ||
        !checkedMultiply(pixelCount, description->channels, sourceSamples) ||
        !checkedMultiply(sourceSamples, sampleBytes, dataBytes) ||
        dataBytes > std::numeric_limits<std::size_t>::max() - header.dataOffset) {
        return readError("ImageHeaderInvalid", "FITS image dimensions exceed supported memory limits");
    }
    const auto neededBytes = header.dataOffset + dataBytes;
    if (static_cast<std::uintmax_t>(neededBytes) > fileSize) {
        return readError("InputReadFailed", "FITS data section is shorter than the image dimensions require");
    }

    const auto bscale = floatingValue(header, "BSCALE", 1.0);
    const auto bzero = floatingValue(header, "BZERO", 0.0);
    if (!bscale.has_value() || !bzero.has_value()) {
        return readError("ImageHeaderInvalid", "FITS scaling values must be finite numbers");
    }

    SampleRange colorRange;
    SampleRange alphaRange;
    const auto colorChannels = static_cast<std::size_t>(std::min<std::uint16_t>(description->channels, 3));
    if (options.mode == FitsDecodeMode::DisplayNormalized) {
        const auto rangeStatus = forEachPhysicalSample(
            file, header.dataOffset, sourceSamples, sampleBytes, description->bitpix, *bscale, *bzero,
            [&](std::size_t sample, float value) {
                const auto channel = sample / pixelCount;
                if (channel < colorChannels) {
                    includeInRange(colorRange, value);
                } else if (channel == 3) {
                    includeInRange(alphaRange, value);
                }
            }
        );
        if (rangeStatus == SampleStreamResult::AllocationFailed) {
            return readError("ImageAllocationFailed", "Could not allocate the FITS decode buffer");
        }
        if (rangeStatus != SampleStreamResult::Success) {
            return readError("InputReadFailed", "Failed while reading the FITS data section");
        }
    }

    const bool displayDomain = header.values.contains("PSCOLOR") && header.values.at("PSCOLOR") == "SRGB-DISPLAY";
    const auto decodeColor = [&](float value) {
        if (displayDomain) return value;
        return options.mode == FitsDecodeMode::Scientific ? value : normalizeForDisplay(value, colorRange);
    };
    const auto decodeAlpha = [&](float value) {
        return displayDomain || options.mode == FitsDecodeMode::Scientific ? value : normalizeForDisplay(value, alphaRange);
    };

    ImageReadResult result;
    result.backend = ImageReadBackend::Fits;
    result.image.width = description->width;
    result.image.height = description->height;
    result.image.channels = 4;
    result.image.format = PixelFormat::Float32RGBA;
    result.image.colorEncoding = displayDomain ? ColorEncoding::SRGB : ColorEncoding::Linear;
    result.image.sourceBitsPerChannel = bitsPerChannelForBitpix(description->bitpix);
    std::size_t outputSamples = 0;
    if (!checkedMultiply(pixelCount, static_cast<std::size_t>(result.image.channels), outputSamples)) {
        return readError("ImageHeaderInvalid", "FITS image dimensions exceed supported memory limits");
    }
    try {
        result.image.pixels.assign(outputSamples, 0.0F);
    } catch (const std::bad_alloc&) {
        return readError("ImageAllocationFailed", "Could not allocate the decoded FITS image buffer");
    } catch (const std::length_error&) {
        return readError("ImageAllocationFailed", "Could not allocate the decoded FITS image buffer");
    }
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        result.image.pixels[pixel * result.image.channels + 3] = 1.0F;
    }

    const auto decodeStatus = forEachPhysicalSample(
        file, header.dataOffset, sourceSamples, sampleBytes, description->bitpix, *bscale, *bzero,
        [&](std::size_t sample, float sourceValue) {
            const auto channel = sample / pixelCount;
            const auto pixel = sample % pixelCount;
            const auto outputOffset = pixel * result.image.channels;
            const bool invalidSource = !std::isfinite(sourceValue);
            if (description->channels == 1) {
                const float value = decodeColor(sourceValue);
                result.image.pixels[outputOffset] = value;
                result.image.pixels[outputOffset + 1] = value;
                result.image.pixels[outputOffset + 2] = value;
            } else if (channel < colorChannels) {
                result.image.pixels[outputOffset + channel] = decodeColor(sourceValue);
            } else if (channel == 3) {
                const bool priorInvalid = options.maskNonFinitePixels &&
                                          !std::isfinite(result.image.pixels[outputOffset + 3]);
                result.image.pixels[outputOffset + 3] = decodeAlpha(sourceValue);
                if (priorInvalid) {
                    result.image.pixels[outputOffset + 3] = std::numeric_limits<float>::quiet_NaN();
                }
            }

            if (options.maskNonFinitePixels && invalidSource) {
                result.image.pixels[outputOffset + 3] = std::numeric_limits<float>::quiet_NaN();
            }
        }
    );
    if (decodeStatus == SampleStreamResult::AllocationFailed) {
        return readError("ImageAllocationFailed", "Could not allocate the FITS decode buffer");
    }
    if (decodeStatus != SampleStreamResult::Success) {
        return readError("InputReadFailed", "Failed while reading the FITS data section");
    }

    if (sensorPattern) {
        const auto& model = *sensorPattern;
        if (description->channels != 1 || !header.values.contains("BAYERPAT") ||
            model.width != result.image.width || model.height != result.image.height || model.channels != 1 ||
            model.colorEncoding != ColorEncoding::Linear || model.pixels.size() != pixelCount)
            return readError("SensorPatternInvalid", "Sensor pattern requires a matching linear grayscale model and CFA input");
        for (std::size_t p = 0; p < pixelCount; ++p) {
            if (!std::isfinite(model.pixels[p])) return readError("SensorPatternInvalid", "Nonfinite sensor pattern value");
            if (!std::isfinite(result.image.pixels[p * 4 + 3]) || result.image.pixels[p * 4 + 3] <= 0) continue;
            for (unsigned c = 0; c < 3; ++c) {
                result.image.pixels[p * 4 + c] -= model.pixels[p];
                if (!std::isfinite(result.image.pixels[p * 4 + c]))
                    return readError("SensorPatternInvalid", "Sensor correction exceeds finite sample range");
            }
        }
    }
    if (calibration) {
        if (options.mode != FitsDecodeMode::Scientific)
            return readError("ScientificImageRequired", "Sensor calibration requires scientific FITS decoding");
        if (options.maskNonFinitePixels) {
            for (std::size_t p = 0; p < pixelCount; ++p) {
                if (!std::isfinite(result.image.pixels[p * 4 + 3])) {
                    for (int c = 0; c < 4; ++c) result.image.pixels[p * 4 + c] = 0;
                }
            }
        }
        auto calibrated = Calibrator().calibrate(result.image, *calibration);
        if (!calibrated.ok) return readError(calibrated.errorCode, calibrated.message);
        result.image = std::move(calibrated.image);
    }

    if (options.debayer && description->channels == 1 && header.values.contains("BAYERPAT")) {
        const detail::StageScope demosaicTimer(detail::Stage::Demosaic);
        for (float gain : options.cfaInterpolationGains)
            if (!std::isfinite(gain) || gain <= 0 || gain > 20)
                return readError("ArgumentInvalid", "CFA interpolation gains must be finite, positive and at most 20");
        auto pattern = header.values.at("BAYERPAT");
        std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (pattern != "RGGB" && pattern != "GRBG" && pattern != "GBRG" && pattern != "BGGR") {
            return readError("BayerPatternUnsupported", "Unsupported FITS BAYERPAT: " + pattern);
        }
        const auto xOffset = integerValue(header, "XBAYROFF");
        const auto yOffset = integerValue(header, "YBAYROFF");
        if ((header.values.contains("XBAYROFF") && !xOffset) || (header.values.contains("YBAYROFF") && !yOffset)) {
            return readError("ImageHeaderInvalid", "FITS Bayer offsets must be integers");
        }
        if (result.image.width < 2 || result.image.height < 2) {
            return readError("ImageTooSmall", "Bayer images must be at least 2 by 2 pixels");
        }
        // Preserve the sensor samples separately so interpolation never feeds on
        // already interpolated pixels. Coordinates follow the stored FITS rows.
        std::vector<float> mosaic;
        try {
            mosaic.resize(pixelCount);
        } catch (const std::bad_alloc&) {
            return readError("ImageAllocationFailed", "Could not allocate the Bayer sample buffer");
        }
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            mosaic[pixel] = std::isfinite(result.image.pixels[pixel * 4 + 3]) && result.image.pixels[pixel * 4 + 3] > 1.e-6F
                                ? result.image.pixels[pixel * 4]
                                : std::numeric_limits<float>::quiet_NaN();
        }
        // FITS offsets shift the four CFA phases once for the entire exposure.
        // The interpolation stencil reads these phases repeatedly for each pixel.
        const unsigned originX = static_cast<unsigned>(xOffset.value_or(0)) & 1U;
        const unsigned originY = static_cast<unsigned>(yOffset.value_or(0)) & 1U;
        std::array<int, 4> colors;
        for (unsigned phase = 0; phase < colors.size(); ++phase) {
            const char color = pattern[phase ^ (2 * originY + originX)];
            colors[phase] = color == 'R' ? 0 : color == 'G' ? 1 : 2;
        }
        const auto colorAt = [&](unsigned x, unsigned y) {
            return colors[2 * (y & 1U) + (x & 1U)];
        };
        bool reconstructedOnGPU = false;
        std::vector<float> scientificRGB;
        if (options.preferMetal) {
#ifdef __APPLE__
            // With 16-bit integer samples and unity gains the Malvar stencil
            // sums (including half coefficients) are exactly representable in
            // float. Other scientific units or masks use the CPU double kernel.
            if (!calibration && !sensorPattern && options.mode == FitsDecodeMode::Scientific &&
                description->bitpix == 16 && *bscale == 1 && (*bzero == 0 || *bzero == 32768) &&
                options.cfaInterpolationGains == std::array<float,3>{1,1,1} &&
                (options.demosaic == FitsDemosaic::Malvar || options.demosaic == FitsDemosaic::MalvarGuarded) &&
                result.image.width >= 4 && result.image.height >= 4 && pixelCount >= 65536 && pixelCount <= UINT32_MAX)
                reconstructedOnGPU = detail::metalDemosaic(mosaic, colors, result.image, options.demosaic == FitsDemosaic::MalvarGuarded);
            else if (options.mode == FitsDecodeMode::Scientific &&
                     (options.demosaic == FitsDemosaic::Malvar || options.demosaic == FitsDemosaic::MalvarGuarded) &&
                     result.image.width >= 4 && result.image.height >= 4 && pixelCount >= 65536 && pixelCount <= UINT32_MAX) {
                detail::metalScientificCFA(mosaic,result.image.width,result.image.height,colors,
                                           options.cfaInterpolationGains,scientificRGB);
            } else detail::recordBackend("cpu", "Metal CFA requires full-size scientific Malvar interpolation");
#else
            detail::recordBackend("cpu", "Metal unavailable on this platform");
#endif
        }
        if (!reconstructedOnGPU)
        detail::parallelRanges(result.image.height, std::max<std::size_t>(1, 262144 / result.image.width),
            [&](std::size_t firstRow, std::size_t lastRow) noexcept {
                for (int y = static_cast<int>(firstRow); y < static_cast<int>(lastRow); ++y) {
                    for (int x = 0; x < static_cast<int>(result.image.width); ++x) {
                        const auto pixel = static_cast<std::size_t>(y) * result.image.width + x;
                        if (!scientificRGB.empty() && std::isfinite(scientificRGB[pixel*4])) {
                            std::copy_n(scientificRGB.data()+pixel*4,3,result.image.pixels.data()+pixel*4);
                            continue;
                        }
                        for (int channel = 0; channel < 3; ++channel) {
                            if (colorAt(x, y) == channel) {
                                result.image.pixels[pixel * 4 + channel] = mosaic[pixel];
                                continue;
                            }
                            // Malvar-He-Cutler (ICASSP 2004), Figure 2. Keep measured
                            // samples and signed scientific values. At borders or near
                            // invalid samples, use the finite-neighbour bilinear fallback.
                            if ((options.demosaic == FitsDemosaic::Malvar || options.demosaic == FitsDemosaic::Ratio ||
                                 options.demosaic == FitsDemosaic::MalvarGuarded) && x >= 2 && y >= 2 &&
                                x + 2 < static_cast<int>(result.image.width) &&
                                y + 2 < static_cast<int>(result.image.height)) {
                                const auto v = [&](int dx, int dy) -> double {
                                    return static_cast<double>(mosaic[static_cast<std::size_t>(y + dy) * result.image.width + x + dx]) *
                                           options.cfaInterpolationGains[colorAt(x + dx, y + dy)];
                                };
                                const double center = v(0, 0);
                                const double cross2 = v(-2, 0) + v(2, 0) + v(0, -2) + v(0, 2);
                                double value;
                                if (channel == 1) {
                                    value = (4 * center + 2 * (v(-1, 0) + v(1, 0) + v(0, -1) + v(0, 1)) - cross2) / 8;
                                } else if (colorAt(x, y) != 1) {
                                    value = (6 * center + 2 * (v(-1, -1) + v(1, -1) + v(-1, 1) + v(1, 1)) - 1.5 * cross2) / 8;
                                } else {
                                    const bool horizontal = colorAt(x + 1, y) == channel;
                                    const double near = horizontal ? v(-1, 0) + v(1, 0) : v(0, -1) + v(0, 1);
                                    const double along = horizontal ? v(-2, 0) + v(2, 0) : v(0, -2) + v(0, 2);
                                    const double across = horizontal ? v(0, -2) + v(0, 2) : v(-2, 0) + v(2, 0);
                                    value = (5 * center + 4 * near - along + .5 * across -
                                             v(-1, -1) - v(1, -1) - v(-1, 1) - v(1, 1)) / 8;
                                }
                                value /= options.cfaInterpolationGains[channel];
                                if (std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()) {
                                    result.image.pixels[pixel * 4 + channel] = static_cast<float>(value);
                                    continue;
                                }
                            }
                            double sum = 0.0;
                            int count = 0;
                            for (int dy = -1; dy <= 1; ++dy) {
                                for (int dx = -1; dx <= 1; ++dx) {
                                    const int sx = x + dx, sy = y + dy;
                                    if (sx < 0 || sy < 0 || sx >= static_cast<int>(result.image.width) ||
                                        sy >= static_cast<int>(result.image.height) || colorAt(sx, sy) != channel) {
                                        continue;
                                    }
                                    const float value = mosaic[static_cast<std::size_t>(sy) * result.image.width + sx];
                                    if (std::isfinite(value)) {
                                        sum += value;
                                        ++count;
                                    }
                                }
                            }
                            result.image.pixels[pixel * 4 + channel] =
                                count > 0 ? static_cast<float>(sum / count) : std::numeric_limits<float>::quiet_NaN();
                            if (count == 0 && options.maskNonFinitePixels) {
                                result.image.pixels[pixel * 4 + 3] = std::numeric_limits<float>::quiet_NaN();
                            }
                        }
                    }
                }
        });
        if (options.demosaic == FitsDemosaic::MalvarGuarded) {
            try {
                if (!detail::guardDemosaicStars(result.image, mosaic, colors, options.cfaInterpolationGains,
                                               replay, options.maskNonFinitePixels))
                    return readError("StarGuardReplayFailed", "Stellar repair could not be replayed");
            } catch (const std::bad_alloc&) {
                return readError("ImageAllocationFailed", "Could not allocate stellar interpolation guards");
            }
        }
        if (options.demosaic == FitsDemosaic::Ratio) {
            try {
                const auto reconstructed = detail::demosaicRatio(mosaic,
                    static_cast<int>(result.image.width), static_cast<int>(result.image.height),
                    colors, options.cfaInterpolationGains, result.image.pixels);
                for (std::size_t p = 0; p < pixelCount; ++p)
                    for (int c = 0; c < 3; ++c)
                        if (std::isfinite(reconstructed[p * 3 + c]))
                            result.image.pixels[p * 4 + c] = reconstructed[p * 3 + c];
            } catch (const std::bad_alloc&) {
                return readError("ImageAllocationFailed", "Could not allocate ratio Bayer reconstruction buffers");
            }
        }
        if (options.demosaic == FitsDemosaic::Menon) {
            std::array<int, 4> colors{colorAt(0,0),colorAt(1,0),colorAt(0,1),colorAt(1,1)};
            try {
                const auto reconstructed = detail::demosaicMenon(mosaic,
                    static_cast<int>(result.image.width), static_cast<int>(result.image.height),
                    colors, options.cfaInterpolationGains);
                for (std::size_t p=0;p<pixelCount;++p)
                    for (int c=0;c<3;++c)
                        if (std::isfinite(reconstructed[p*3+c])) result.image.pixels[p*4+c]=reconstructed[p*3+c];
            } catch (const std::bad_alloc&) {
                return readError("ImageAllocationFailed", "Could not allocate directional Bayer reconstruction buffers");
            }
        }

    }

    if (options.maskNonFinitePixels) {
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const auto outputOffset = pixel * result.image.channels;
            if (!std::isfinite(result.image.pixels[outputOffset + 3]) || result.image.pixels[outputOffset + 3] <= 0) {
                result.image.pixels[outputOffset] = 0.0F;
                result.image.pixels[outputOffset + 1] = 0.0F;
                result.image.pixels[outputOffset + 2] = 0.0F;
                result.image.pixels[outputOffset + 3] = 0.0F;
            }
        }
    }

    result.ok = true;
    return result;
}

ImageReadResult FitsCodec::read(const std::filesystem::path& path, const FitsDecodeOptions& options,
                               const CalibrationOptions* calibration, const ImageBuffer* sensorPattern) const {
    return readFits(path, options, calibration, sensorPattern, nullptr);
}

ImageReadResult detail::readAnalysisFits(const std::filesystem::path& path, const FitsDecodeOptions& options,
                                         GuardReplay& replay) {
    replay.use_ = GuardReplay::Use::unused;
    auto result = readFits(path, options, nullptr, nullptr,
                           options.mode == FitsDecodeMode::Scientific ? &replay : nullptr);
    if (result.ok && replay.use_ != GuardReplay::Use::unused) return result;
    const bool retry = replay.replayedLastRead();
    replay.discard();
    // A failed replay may have edited earlier apertures in its private image.
    // Start again with a fresh source decode before using the complete guard.
    if (retry) return FitsCodec().read(path, options);
    return result;
}

namespace {
void appendWcsCards(std::vector<unsigned char>& header, const TanGrid* sky, const std::vector<SipTerm>* distortion, unsigned channels) {
    if (!std::isfinite(sky->x) || !std::isfinite(sky->y) || !std::isfinite(sky->ra) || !std::isfinite(sky->dec) || std::abs(sky->dec)>=90 ||
        !std::all_of(sky->cd.begin(),sky->cd.end(),[](double v){return std::isfinite(v);}) ||
        std::abs(sky->cd[0]*sky->cd[3]-sky->cd[1]*sky->cd[2])<1e-18)
        throw std::invalid_argument("Invalid celestial WCS grid");
    (void)SkyProjection(SolvedPanel{*sky, distortion ? *distortion : std::vector<SipTerm>{}}, *sky);
    auto put = [&](const char* key, double n) {
        std::ostringstream value;
        value << std::uppercase << std::setprecision(16) << n;
        appendCard(header, makeCard(key, value.str()));
    };
    for (auto [key, value] : {std::pair{"CTYPE1", distortion && !distortion->empty() ? "'RA---TAN-SIP'" : "'RA---TAN'"},
                              {"CTYPE2", distortion && !distortion->empty() ? "'DEC--TAN-SIP'" : "'DEC--TAN'"},
                              {"CUNIT1", "'deg'"},
                              {"CUNIT2", "'deg'"},
                              {"RADESYS", "'ICRS'"}})
        appendCard(header, makeCard(key, value));
    put("CRPIX1", sky->x);
    put("CRPIX2", sky->y);
    put("CRVAL1", sky->ra);
    put("CRVAL2", sky->dec);
    put("CD1_1", sky->cd[0]);
    put("CD1_2", sky->cd[1]);
    put("CD2_1", sky->cd[2]);
    put("CD2_2", sky->cd[3]);
    if (distortion && !distortion->empty()) {
        unsigned order=2;
        std::set<std::pair<unsigned,unsigned>> powers;
        for (const auto& t:*distortion) {
            if(t.i>5||t.j>5||t.i+t.j<2||t.i+t.j>5||!std::isfinite(t.a)||!std::isfinite(t.b)||
               !powers.emplace(t.i,t.j).second)
                throw std::invalid_argument("SIP terms must be unique finite terms of total degree 2...5");
            order=std::max(order,t.i+t.j);
            put(("A_"+std::to_string(t.i)+"_"+std::to_string(t.j)).c_str(),t.a);
            put(("B_"+std::to_string(t.i)+"_"+std::to_string(t.j)).c_str(),t.b);
        }
        put("A_ORDER",order);put("B_ORDER",order);
    }
    if (channels != 1)
        put("CD3_3", 1);

}
} // namespace

void copyFitsWithWcs(const std::filesystem::path& source, const std::filesystem::path& output, const SolvedPanel& sky) {
    if (std::filesystem::exists(output) || std::filesystem::is_symlink(output))
        throw std::invalid_argument("WCS output must be a new file");
    std::ifstream in(source, std::ios::binary);
    const auto parsed = parseHeader(in);
    if (!parsed.ok || parsed.dataOffset > 1024 * 1024) throw std::invalid_argument("Invalid or oversized FITS header");
    const auto description = imageDescription(parsed);
    if (!description || (description->channels != 1 && description->channels != 3 && description->channels != 4) || parsed.values.contains("BAYERPAT"))
        throw std::invalid_argument("WCS attachment requires a mono/RGB master");
    const auto bytes = std::uint64_t(description->width) * description->height;
    const auto stride = std::uint64_t(description->channels) * unsigned(std::abs(description->bitpix)/8);
    const auto fileSize = std::filesystem::file_size(source);
    if(fileSize < parsed.dataOffset || bytes > (fileSize-parsed.dataOffset)/stride)
        throw std::invalid_argument("Truncated FITS image payload");
    // Remove all prior primary and alternate WCS descriptions, including old
    // inverse SIP / lookup distortions. Preserve acquisition cards verbatim.
    const auto isWcs = [](const std::string& key) {
        for (const auto* prefix : {"CTYPE", "CUNIT", "CRPIX", "CRVAL", "CDELT", "CROTA", "WCSAXES", "WCSNAME",
                "RADESYS", "RADECSYS", "EQUINOX", "LONPOLE", "LATPOLE", "CPDIS", "CQDIS", "D2IM", "DET2IM",
                "A_", "B_", "AP_", "BP_", "LTM", "LTV"})
            if (key.starts_with(prefix)) return true;
        for (const auto* prefix : {"CD", "PC", "PV", "PS", "DP", "DQ"})
            if (key.starts_with(prefix) && key.size() > 2 && std::isdigit(static_cast<unsigned char>(key[2]))) return true;
        return key == "CHECKSUM" || key == "DATASUM";
    };
    in.clear(); in.seekg(0);
    std::vector<unsigned char> header;
    for (std::size_t offset=0; offset<parsed.dataOffset; offset+=80) {
        std::string card(80, ' '); in.read(card.data(),80);
        if (!in) throw std::runtime_error("Cannot read FITS header");
        const auto key=trim(card.substr(0,8)); if(key=="END")break;
        if(!isWcs(key)) appendCard(header,card);
    }
    appendWcsCards(header,&sky.grid,&sky.distortion,description->channels);
    appendCard(header,makeEndCard());
    header.resize((header.size()+2879)/2880*2880,' ');
    in.clear();in.seekg(static_cast<std::streamoff>(parsed.dataOffset));
    auto* out=std::fopen(output.c_str(),"wbx");
    if(!out)throw std::runtime_error("Cannot create new WCS output");
    try {
        if(std::fwrite(header.data(),1,header.size(),out)!=header.size())throw std::runtime_error("Cannot write WCS header");
        std::array<char,65536> buffer{};
        while(in) {
            in.read(buffer.data(),buffer.size());const auto count=static_cast<std::size_t>(in.gcount());
            if(count&&std::fwrite(buffer.data(),1,count,out)!=count)throw std::runtime_error("Cannot copy FITS data");
        }
        if(!in.eof()||std::fflush(out))throw std::runtime_error("Cannot save WCS output");
        const auto status=std::fclose(out);out=nullptr;if(status)throw std::runtime_error("Cannot close WCS output");
    } catch(...) {if(out)std::fclose(out);std::error_code ec;std::filesystem::remove(output,ec);throw;}

}

static ImageWriteResult writeFitsProduct(const ImageBuffer& image, const std::filesystem::path& path,
                                  const TanGrid* sky, const std::vector<SipTerm>* distortion, FitsOutputDomain domain, const SensorMasterResult* sensor) {
    if (image.empty() || image.channels == 0 || image.channels > 4 || image.pixels.size() != image.sampleCount()) {
        return writeError("ImageBufferInvalid", "Image buffer must be non-empty Float32 data with 1 to 4 channels");
    }

    const bool display = domain == FitsOutputDomain::DisplaySRGB;
    if (domain != FitsOutputDomain::LinearScience && !display)
        return writeError("ImageEncodingInvalid", "Unknown FITS output domain");
    if (display && (image.colorEncoding != ColorEncoding::SRGB ||
        !std::all_of(image.pixels.begin(),image.pixels.end(),[](float v){return std::isfinite(v)&&v>=0&&v<=1;})))
        return writeError("ImageEncodingInvalid", "Display FITS requires bounded sRGB values");
    std::vector<unsigned char> header;
    try {
        appendCard(header, makeCard("SIMPLE", "T"));
        appendCard(header, makeCard("BITPIX", "-32"));
        appendCard(header, makeCard("NAXIS", image.channels == 1 ? "2" : "3"));
        appendCard(header, makeCard("NAXIS1", std::to_string(image.width)));
        appendCard(header, makeCard("NAXIS2", std::to_string(image.height)));
        if (image.channels != 1) {
            appendCard(header, makeCard("NAXIS3", std::to_string(image.channels)));
        }
        appendCard(header, makeCard("BSCALE", "1.0"));
        appendCard(header, makeCard("BZERO", "0.0"));
        appendCard(header, makeCard("PSCOLOR", sensor ? "'Linear'" : display ? "'SRGB-DISPLAY'" : "'LINEAR-SRGB'"));
        if (sensor) {
            auto str = [&](const std::string& key, const std::string& value) {
                std::string quoted = "'";
                for (unsigned char c : value) {
                    if (c < 32 || c > 126) throw std::invalid_argument("Sensor metadata requires printable FITS ASCII");
                    quoted += char(c); if (c == '\'') quoted += '\'';
                }
                quoted += "'";
                if (quoted.size() > 68) throw std::invalid_argument("Sensor metadata exceeds one FITS card");
                appendCard(header, makeCard(key, quoted));
            };
            auto num = [&](const std::string& key, double value) {
                if (!std::isfinite(value)) throw std::invalid_argument("Invalid sensor metadata number");
                std::ostringstream out; out << std::setprecision(17) << value;
                appendCard(header, makeCard(key, out.str()));
            };
            const auto& a = sensor->acquisition;
            for (const auto& [field, key] : std::map<std::string, std::string>{
                     {"camera","INSTRUME"},{"cameraSerial","CAM-SER"},{"readoutMode","READMODE"},
                     {"units","BUNIT"},{"filter","FILTER"},{"opticalTrain","PSOPTICS"}})
                if (a.text.contains(field)) str(key, a.text.at(field));
            for (const auto& [field, key] : std::map<std::string, std::string>{
                     {"exposure","EXPTIME"},{"gain","GAIN"},{"iso","ISOSPEED"},{"offset","OFFSET"},
                     {"binX","XBINNING"},{"binY","YBINNING"},{"originX","XORGSUBF"},{"originY","YORGSUBF"},
                     {"electronsPerADU","EGAIN"},{"focalLength","FOCALLEN"},{"sensorTemperature","CCD-TEMP"},
                     {"sensorTemperatureMin","PSTMIN"},{"sensorTemperatureMax","PSTMAX"}})
                if (a.numeric.contains(field)) num(key,a.numeric.at(field));
            const auto sampling = a.text.at("sampling");
            if (sampling == "mono") str("PSLAYOUT","MONO");
            else { str("PSLAYOUT","CFA"); str("BAYERPAT",sampling.substr(4)); num("XBAYROFF",0); num("YBAYROFF",0); }
            str("IMAGETYP", "Master " + sensor->role);
            str("PSBIAS",sensor->biasState); str("PSMETHOD",sensor->method);
            num("STACKCNT",sensor->sources.size());
            str("PSNORM",sensor->role == "flat" ? "phase-mean" : "none");
        }

        if (sky) appendWcsCards(header, sky, distortion, image.channels);
        appendCard(header, makeEndCard());
        header.resize(((header.size() + kFitsBlockSize - 1) / kFitsBlockSize) * kFitsBlockSize, ' ');
    } catch (const std::invalid_argument& e) {
        return writeError("WCSInvalid", e.what());
    } catch (const std::bad_alloc&) {
        return writeError("ImageAllocationFailed", "Could not allocate the FITS header buffer");
    } catch (const std::length_error&) {
        return writeError("ImageAllocationFailed", "Could not allocate the FITS header buffer");
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return writeError("OutputPathInvalid", "Could not open output FITS file");
    }
    file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!file) {
        return writeError("ImageEncodeFailed", "Failed while writing FITS file");
    }

    std::vector<std::uint32_t> chunk;
    try {
        chunk.resize(kFitsStreamChunkBytes / sizeof(float));
    } catch (const std::bad_alloc&) {
        return writeError("ImageAllocationFailed", "Could not allocate the FITS encode buffer");
    }
    const auto pixels = image.pixelCount();
    for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
        for (std::size_t start = 0; start < pixels; start += chunk.size()) {
            const auto count = std::min(chunk.size(), pixels - start);
            for (std::size_t i = 0; i < count; ++i) {
                const auto offset = (start + i) * image.channels + channel;
                const auto value = display ? image.pixels[offset] : sampleForFits(image, channel, offset);
                const auto bits = std::bit_cast<std::uint32_t>(value);
                if constexpr (std::endian::native == std::endian::big) chunk[i] = bits;
                else chunk[i] = (bits >> 24) | ((bits >> 8) & 0xff00u) | ((bits << 8) & 0xff0000u) | (bits << 24);
            }
            file.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(count * sizeof(float)));
            if (!file) return writeError("ImageEncodeFailed", "Failed while writing FITS file");
        }
    }

    std::size_t payloadBytes = 0;
    if (!checkedMultiply(image.sampleCount(), sizeof(float), payloadBytes)) {
        return writeError("ImageBufferInvalid", "Image buffer exceeds supported FITS dimensions");
    }
    const auto paddingBytes = (kFitsBlockSize - payloadBytes % kFitsBlockSize) % kFitsBlockSize;
    if (paddingBytes != 0) {
        std::array<unsigned char, kFitsBlockSize> padding{};
        file.write(reinterpret_cast<const char*>(padding.data()), static_cast<std::streamsize>(paddingBytes));
    }
    if (!file) {
        return writeError("ImageEncodeFailed", "Failed while writing FITS file");
    }

    ImageWriteResult result;
    result.ok = true;
    return result;
}

ImageWriteResult FitsCodec::write(const ImageBuffer& image, const std::filesystem::path& path,
                                  const TanGrid* sky, const std::vector<SipTerm>* distortion, FitsOutputDomain domain) const {
    return writeFitsProduct(image, path, sky, distortion, domain, nullptr);
}
void writeSensorMaster(const SensorMasterResult& result, const std::filesystem::path& path) {
    if (result.master.channels != 1 || result.master.colorEncoding != ColorEncoding::Linear ||
        result.master.width != result.acquisition.width || result.master.height != result.acquisition.height ||
        result.sources.size() < 2 || result.sources.size() > 256 ||
        !std::set<std::string>{"bias","dark","dark-flat","flat"}.contains(result.role) ||
        !std::set<std::string>{"included","removed"}.contains(result.biasState))
        throw std::invalid_argument("Invalid sensor master product");
    const auto written = writeFitsProduct(result.master, path, nullptr, nullptr, FitsOutputDomain::LinearScience, &result);
    if (!written.ok) throw std::runtime_error(written.message);
}

void writeCalibratedSensor(const SensorCalibrationResult& result, const std::filesystem::path& path) {
    const auto& image = result.calibrated;
    if (image.empty() || image.channels != 1 || image.colorEncoding != ColorEncoding::Linear ||
        image.width != result.acquisition.width || image.height != result.acquisition.height ||
        image.pixels.size() != image.pixelCount() || result.input.sha.empty() || fileSHA256(result.input.path) != result.input.sha)
        throw std::invalid_argument("Invalid or changed calibrated sensor source");
    std::ifstream in(result.input.path, std::ios::binary);
    const auto parsed = parseHeader(in);
    const auto description = imageDescription(parsed);
    if (!parsed.ok || parsed.dataOffset > 1024 * 1024 || !description || description->channels != 1 ||
        description->width != image.width || description->height != image.height)
        throw std::invalid_argument("Invalid calibrated sensor header");
    // No resampling: sky and sensor-coordinate cards remain valid. Storage
    // scaling, checksum, count and range cards no longer describe the pixels.
    const std::set<std::string> replaced{"BITPIX", "BSCALE", "BZERO", "BLANK", "CHECKSUM", "DATASUM", "DATAMIN", "DATAMAX",
        "NEXTEND", "STACKCNT", "PSCOLOR", "PSBIAS", "PSCALIB", "PSNORM"};
    std::vector<unsigned char> header;
    in.clear(); in.seekg(0);
    for (std::size_t offset = 0; offset < parsed.dataOffset; offset += 80) {
        std::string card(80, ' '); in.read(card.data(), 80);
        if (!in) throw std::runtime_error("Cannot read sensor source header");
        const auto key = trim(card.substr(0, 8)); if (key == "END") break;
        if (key == "BITPIX") appendCard(header, makeCard("BITPIX", "-32"));
        else if (!replaced.contains(key)) appendCard(header, card);
    }
    appendCard(header, makeCard("BSCALE", "1")); appendCard(header, makeCard("BZERO", "0"));
    appendCard(header, makeCard("PSCOLOR", "'Linear'")); appendCard(header, makeCard("PSBIAS", "'removed'"));
    appendCard(header, makeCard("PSCALIB", "'sensor-v1'"));
    appendCard(header, makeEndCard()); header.resize((header.size() + 2879) / 2880 * 2880, ' ');
    auto* out = std::fopen(path.c_str(), "wbx");
    if (!out) throw std::runtime_error("Choose a new calibrated output file");
    try {
        if (std::fwrite(header.data(), 1, header.size(), out) != header.size()) throw std::runtime_error("Cannot write calibrated header");
        std::vector<std::uint32_t> block(16384);
        for (std::size_t start = 0; start < image.pixels.size(); start += block.size()) {
            if (result.options.checkCancelled) result.options.checkCancelled();
            const auto n = std::min(block.size(), image.pixels.size() - start);
            for (std::size_t p = 0; p < n; ++p) {
                const auto bits = std::bit_cast<std::uint32_t>(image.pixels[start + p]);
                if constexpr (std::endian::native == std::endian::big) block[p] = bits;
                else block[p] = (bits >> 24) | ((bits >> 8) & 0xff00u) | ((bits << 8) & 0xff0000u) | (bits << 24);
            }
            if (std::fwrite(block.data(), 4, n, out) != n) throw std::runtime_error("Cannot write calibrated pixels");
        }
        const auto padding = (2880 - (image.pixels.size() * 4) % 2880) % 2880;
        const std::array<char, 2880> zeros{};
        if (std::fwrite(zeros.data(), 1, padding, out) != padding || std::fflush(out)) throw std::runtime_error("Cannot save calibrated pixels");
        const auto status = std::fclose(out); out = nullptr;
        if (status || fileSHA256(result.input.path) != result.input.sha) throw std::runtime_error("Calibrated source changed during output");
    } catch (...) { if (out) std::fclose(out); std::error_code ec; std::filesystem::remove(path, ec); throw; }
}

} // namespace photonstack
