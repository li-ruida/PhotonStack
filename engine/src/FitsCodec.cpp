#include "photonstack/FitsCodec.hpp"

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

    if (options.maskNonFinitePixels) {
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const auto outputOffset = pixel * result.image.channels;
            if (!std::isfinite(result.image.pixels[outputOffset + 3])) {
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
