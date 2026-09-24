#include "photonstack/FitsCodec.hpp"
#include "DemosaicMenon.hpp"
#include "DemosaicRatio.hpp"
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

void appendFloat32BigEndian(std::vector<unsigned char>& data, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::copy(reinterpret_cast<const unsigned char*>(&value), reinterpret_cast<const unsigned char*>(&value) + 4,
              reinterpret_cast<unsigned char*>(&bits));
    data.push_back(static_cast<unsigned char>((bits >> 24U) & 0xFFU));
    data.push_back(static_cast<unsigned char>((bits >> 16U) & 0xFFU));
    data.push_back(static_cast<unsigned char>((bits >> 8U) & 0xFFU));
    data.push_back(static_cast<unsigned char>(bits & 0xFFU));
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

bool isFitsPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".fits" || extension == ".fit" || extension == ".fts";
}

bool FitsSensorIdentity::sameAcquisition(const FitsSensorIdentity& b) const {
    return ok && b.ok && width == b.width && height == b.height && camera == b.camera &&
        bayer == b.bayer && xOffset == b.xOffset && yOffset == b.yOffset && exposure == b.exposure && gain == b.gain;
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

ImageReadResult FitsCodec::read(const std::filesystem::path& path, const FitsDecodeOptions& options,
                               const CalibrationOptions* calibration, const ImageBuffer* sensorPattern) const {
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
        const auto colorAt = [&](int x, int y) {
            const auto px = (x + (xOffset.value_or(0) % 2) + 2) % 2;
            const auto py = (y + (yOffset.value_or(0) % 2) + 2) % 2;
            const char color = pattern[static_cast<std::size_t>(py * 2 + px)];
            return color == 'R' ? 0 : color == 'G' ? 1 : 2;
        };
        detail::parallelRanges(result.image.height, std::max<std::size_t>(1, 262144 / result.image.width),
            [&](std::size_t firstRow, std::size_t lastRow) noexcept {
                for (int y = static_cast<int>(firstRow); y < static_cast<int>(lastRow); ++y) {
                    for (int x = 0; x < static_cast<int>(result.image.width); ++x) {
                        const auto pixel = static_cast<std::size_t>(y) * result.image.width + x;
                        for (int channel = 0; channel < 3; ++channel) {
                            if (colorAt(x, y) == channel) {
                                result.image.pixels[pixel * 4 + channel] = mosaic[pixel];
                                continue;
                            }
                            // Malvar-He-Cutler (ICASSP 2004), Figure 2. Keep measured
                            // samples and signed scientific values. At borders or near
                            // invalid samples, use the finite-neighbour bilinear fallback.
                            if ((options.demosaic == FitsDemosaic::Malvar || options.demosaic == FitsDemosaic::Ratio) && x >= 2 && y >= 2 &&
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
        if (options.demosaic == FitsDemosaic::Ratio) {
            const std::array<int, 4> colors{colorAt(0, 0), colorAt(1, 0), colorAt(0, 1), colorAt(1, 1)};
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

ImageWriteResult FitsCodec::write(const ImageBuffer& image, const std::filesystem::path& path) const {
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

    std::vector<unsigned char> payloadChunk;
    try {
        payloadChunk.reserve(kFitsStreamChunkBytes);
    } catch (const std::bad_alloc&) {
        return writeError("ImageAllocationFailed", "Could not allocate the FITS encode buffer");
    } catch (const std::length_error&) {
        return writeError("ImageAllocationFailed", "Could not allocate the FITS encode buffer");
    }
    const auto flushPayload = [&]() {
        if (payloadChunk.empty()) {
            return true;
        }
        file.write(reinterpret_cast<const char*>(payloadChunk.data()),
                   static_cast<std::streamsize>(payloadChunk.size()));
        payloadChunk.clear();
        return static_cast<bool>(file);
    };

    for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                if (payloadChunk.size() + sizeof(float) > kFitsStreamChunkBytes && !flushPayload()) {
                    return writeError("ImageEncodeFailed", "Failed while writing FITS file");
                }
                const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels + channel;
                appendFloat32BigEndian(payloadChunk, sampleForFits(image, channel, offset));
            }
        }
    }
    if (!flushPayload()) {
        return writeError("ImageEncodeFailed", "Failed while writing FITS file");
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
