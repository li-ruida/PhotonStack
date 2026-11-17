#ifdef __APPLE__
#include "MetalCompute.hpp"
#endif
#include <bit>
#include "photonstack/StageProfile.hpp"
#include "photonstack/FitsCodec.hpp"
#include "DemosaicMenon.hpp"
#include "DemosaicRatio.hpp"
#include "DemosaicStarGuard.hpp"
#include "photonstack/ParallelRanges.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
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

FitsHeader parseHeader(std::ifstream& file) {
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

SolvedPanel readSolvedPanel(const std::filesystem::path& path) {
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
    if ((!value("RADESYS").empty() && value("RADESYS") != "ICRS") || h.contains("RADECSYS") || h.contains("EQUINOX") ||
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

bool isFitsPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".fits" || extension == ".fit" || extension == ".fts";
}

bool FitsSensorIdentity::sameAcquisition(const FitsSensorIdentity& b) const {
    return ok && b.ok && width == b.width && height == b.height && camera == b.camera &&
        bayer == b.bayer && xOffset == b.xOffset && yOffset == b.yOffset && exposure == b.exposure && gain == b.gain;
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

    const auto decodeColor = [&](float value) {
        return options.mode == FitsDecodeMode::Scientific ? value : normalizeForDisplay(value, colorRange);
    };
    const auto decodeAlpha = [&](float value) {
        return options.mode == FitsDecodeMode::Scientific ? value : normalizeForDisplay(value, alphaRange);
    };

    ImageReadResult result;
    result.backend = ImageReadBackend::Fits;
    result.image.width = description->width;
    result.image.height = description->height;
    result.image.channels = 4;
    result.image.format = PixelFormat::Float32RGBA;
    result.image.colorEncoding = ColorEncoding::Linear;
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

ImageWriteResult FitsCodec::write(const ImageBuffer& image, const std::filesystem::path& path,
                                  const TanGrid* sky, const std::vector<SipTerm>* distortion) const {
    if (image.empty() || image.channels == 0 || image.channels > 4 || image.pixels.size() != image.sampleCount()) {
        return writeError("ImageBufferInvalid", "Image buffer must be non-empty Float32 data with 1 to 4 channels");
    }

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
        appendCard(header, makeCard("PSCOLOR", "'LINEAR-SRGB'"));
        if (sky) {
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
                        return writeError("WCSInvalid","SIP terms must be unique finite terms of total degree 2...5");
                    order=std::max(order,t.i+t.j);
                    put(("A_"+std::to_string(t.i)+"_"+std::to_string(t.j)).c_str(),t.a);
                    put(("B_"+std::to_string(t.i)+"_"+std::to_string(t.j)).c_str(),t.b);
                }
                put("A_ORDER",order);put("B_ORDER",order);
            }
            if (image.channels != 1)
                put("CD3_3", 1);
        }
        appendCard(header, makeEndCard());
        header.resize(((header.size() + kFitsBlockSize - 1) / kFitsBlockSize) * kFitsBlockSize, ' ');
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
                const auto value = sampleForFits(image, channel, (start + i) * image.channels + channel);
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

} // namespace photonstack
