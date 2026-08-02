#include "photonstack/ImageInspector.hpp"

#include "photonstack/FitsCodec.hpp"
#include "photonstack/RawCodec.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace photonstack {
namespace {

std::uint16_t readBig16(const unsigned char* data) {
    return static_cast<std::uint16_t>((data[0] << 8U) | data[1]);
}

std::uint32_t readBig32(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

std::uint16_t read16(const unsigned char* data, bool littleEndian) {
    if (littleEndian) {
        return static_cast<std::uint16_t>(data[0] | (data[1] << 8U));
    }
    return readBig16(data);
}

std::uint32_t read32(const unsigned char* data, bool littleEndian) {
    if (littleEndian) {
        return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
               (static_cast<std::uint32_t>(data[2]) << 16U) | (static_cast<std::uint32_t>(data[3]) << 24U);
    }
    return readBig32(data);
}

InspectResult error(std::string code, std::string message, const std::filesystem::path& path) {
    InspectResult result;
    result.ok = false;
    result.metadata.path = path.string();
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

bool readAt(std::ifstream& file, std::uintmax_t fileSize, std::uintmax_t offset, unsigned char* destination,
            std::size_t byteCount) {
    if (offset > fileSize || byteCount > fileSize - offset ||
        offset > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(byteCount));
    return file.gcount() == static_cast<std::streamsize>(byteCount);
}

bool isJpegStartOfFrame(unsigned char marker) {
    switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

InspectResult inspectPng(std::ifstream& file, std::uintmax_t fileSize, const std::filesystem::path& path) {
    const std::array<unsigned char, 8> signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::array<unsigned char, 33> data{};
    if (!readAt(file, fileSize, 0, data.data(), data.size()) ||
        !std::equal(signature.begin(), signature.end(), data.begin())) {
        return error("InputFormatUnsupported", "File is not a supported PNG image", path);
    }

    const std::string chunkType(data.begin() + 12, data.begin() + 16);
    if (chunkType != "IHDR") {
        return error("ImageHeaderInvalid", "PNG image is missing an IHDR chunk", path);
    }

    InspectResult result;
    result.ok = true;
    result.metadata.path = path.string();
    result.metadata.format = ImageFormat::PNG;
    result.metadata.width = readBig32(data.data() + 16);
    result.metadata.height = readBig32(data.data() + 20);
    result.metadata.bitsPerChannel = data[24];

    const auto colorType = data[25];
    switch (colorType) {
    case 0:
        result.metadata.channels = 1;
        break;
    case 2:
        result.metadata.channels = 3;
        break;
    case 3:
        result.metadata.channels = 1;
        break;
    case 4:
        result.metadata.channels = 2;
        break;
    case 6:
        result.metadata.channels = 4;
        break;
    default:
        result.metadata.channels = 0;
        break;
    }
    return result;
}

InspectResult inspectJpeg(std::ifstream& file, std::uintmax_t fileSize, const std::filesystem::path& path) {
    std::array<unsigned char, 2> start{};
    if (fileSize < 4 || !readAt(file, fileSize, 0, start.data(), start.size()) || start[0] != 0xFF ||
        start[1] != 0xD8) {
        return error("InputFormatUnsupported", "File is not a supported JPEG image", path);
    }

    std::uintmax_t offset = 2;
    while (offset < fileSize) {
        unsigned char markerByte = 0;
        bool foundMarker = false;
        while (offset < fileSize) {
            if (!readAt(file, fileSize, offset++, &markerByte, 1)) {
                break;
            }
            if (markerByte == 0xFF) {
                foundMarker = true;
                break;
            }
        }
        if (!foundMarker) {
            break;
        }

        do {
            if (offset >= fileSize || !readAt(file, fileSize, offset++, &markerByte, 1)) {
                return error("ImageHeaderInvalid", "JPEG image dimensions were not found", path);
            }
        } while (markerByte == 0xFF);

        const unsigned char marker = markerByte;
        if (marker == 0x00) {
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            break;
        }

        std::array<unsigned char, 2> lengthBytes{};
        if (!readAt(file, fileSize, offset, lengthBytes.data(), lengthBytes.size())) {
            break;
        }
        const std::uint16_t segmentLength = readBig16(lengthBytes.data());
        if (segmentLength < 2 || segmentLength > fileSize - offset) {
            return error("ImageHeaderInvalid", "JPEG segment length is invalid", path);
        }

        if (isJpegStartOfFrame(marker)) {
            if (segmentLength < 8) {
                return error("ImageHeaderInvalid", "JPEG SOF segment is too short", path);
            }
            std::array<unsigned char, 6> frameHeader{};
            if (!readAt(file, fileSize, offset + 2, frameHeader.data(), frameHeader.size())) {
                return error("ImageHeaderInvalid", "JPEG SOF segment is too short", path);
            }

            InspectResult result;
            result.ok = true;
            result.metadata.path = path.string();
            result.metadata.format = ImageFormat::JPEG;
            result.metadata.bitsPerChannel = frameHeader[0];
            result.metadata.height = readBig16(frameHeader.data() + 1);
            result.metadata.width = readBig16(frameHeader.data() + 3);
            result.metadata.channels = frameHeader[5];
            return result;
        }

        offset += segmentLength;
    }

    return error("ImageHeaderInvalid", "JPEG image dimensions were not found", path);
}

InspectResult inspectTiff(std::ifstream& file, std::uintmax_t fileSize, const std::filesystem::path& path) {
    std::array<unsigned char, 8> header{};
    if (!readAt(file, fileSize, 0, header.data(), header.size())) {
        return error("InputFormatUnsupported", "File is not a supported TIFF image", path);
    }

    const bool littleEndian = header[0] == 'I' && header[1] == 'I';
    const bool bigEndian = header[0] == 'M' && header[1] == 'M';
    if (!littleEndian && !bigEndian) {
        return error("InputFormatUnsupported", "File is not a supported TIFF image", path);
    }
    if (read16(header.data() + 2, littleEndian) != 42) {
        return error("InputFormatUnsupported", "TIFF magic number is invalid", path);
    }

    const std::uint32_t ifdOffset = read32(header.data() + 4, littleEndian);
    std::array<unsigned char, 2> countBytes{};
    if (!readAt(file, fileSize, ifdOffset, countBytes.data(), countBytes.size())) {
        return error("ImageHeaderInvalid", "TIFF IFD offset is outside the file", path);
    }

    const std::uint16_t entryCount = read16(countBytes.data(), littleEndian);
    std::uintmax_t entryOffset = static_cast<std::uintmax_t>(ifdOffset) + 2;

    InspectResult result;
    result.ok = true;
    result.metadata.path = path.string();
    result.metadata.format = ImageFormat::TIFF;

    for (std::uint16_t i = 0; i < entryCount; ++i) {
        std::array<unsigned char, 12> entry{};
        if (!readAt(file, fileSize, entryOffset, entry.data(), entry.size())) {
            return error("ImageHeaderInvalid", "TIFF IFD entry is outside the file", path);
        }

        const std::uint16_t tag = read16(entry.data(), littleEndian);
        const std::uint16_t type = read16(entry.data() + 2, littleEndian);
        const std::uint32_t count = read32(entry.data() + 4, littleEndian);
        const std::uint32_t valueOrOffset = read32(entry.data() + 8, littleEndian);

        auto readShortOrLongValue = [&]() -> std::uint32_t {
            if (type == 3 && count == 1) {
                return littleEndian ? (valueOrOffset & 0xFFFFU) : (valueOrOffset >> 16U);
            }
            if (type == 3 && count > 1) {
                std::array<unsigned char, 2> valueBytes{};
                if (readAt(file, fileSize, valueOrOffset, valueBytes.data(), valueBytes.size())) {
                    return read16(valueBytes.data(), littleEndian);
                }
            }
            if (type == 4 && count == 1) {
                return valueOrOffset;
            }
            return 0;
        };

        switch (tag) {
        case 256:
            result.metadata.width = readShortOrLongValue();
            break;
        case 257:
            result.metadata.height = readShortOrLongValue();
            break;
        case 258:
            result.metadata.bitsPerChannel = static_cast<std::uint16_t>(readShortOrLongValue());
            break;
        case 277:
            result.metadata.channels = static_cast<std::uint16_t>(readShortOrLongValue());
            break;
        default:
            break;
        }

        entryOffset += 12;
    }

    if (result.metadata.width == 0 || result.metadata.height == 0) {
        return error("ImageHeaderInvalid", "TIFF image dimensions were not found", path);
    }
    return result;
}

#ifdef __APPLE__
template <typename T> class CFRef {
  public:
    explicit CFRef(T value = nullptr) : value_(value) {}
    ~CFRef() {
        if (value_ != nullptr) {
            CFRelease(value_);
        }
    }

    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;

    [[nodiscard]] T get() const { return value_; }

  private:
    T value_;
};

CFRef<CFURLRef> createFileUrl(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).string();
    CFRef<CFStringRef> cfPath(CFStringCreateWithCString(nullptr, absolute.c_str(), kCFStringEncodingUTF8));
    if (cfPath.get() == nullptr) {
        return CFRef<CFURLRef>();
    }
    return CFRef<CFURLRef>(CFURLCreateWithFileSystemPath(nullptr, cfPath.get(), kCFURLPOSIXPathStyle, false));
}

CFTypeRef dictionaryValue(CFDictionaryRef dictionary, CFStringRef key) {
    if (dictionary == nullptr) {
        return nullptr;
    }
    return CFDictionaryGetValue(dictionary, key);
}

CFDictionaryRef dictionaryValueAsDictionary(CFDictionaryRef dictionary, CFStringRef key) {
    auto value = dictionaryValue(dictionary, key);
    if (value == nullptr || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
        return nullptr;
    }
    return static_cast<CFDictionaryRef>(value);
}

std::string stringValue(CFDictionaryRef dictionary, CFStringRef key) {
    auto value = dictionaryValue(dictionary, key);
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
        return {};
    }

    auto string = static_cast<CFStringRef>(value);
    if (const char* cString = CFStringGetCStringPtr(string, kCFStringEncodingUTF8)) {
        return cString;
    }

    const CFIndex length = CFStringGetLength(string);
    const CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string buffer(static_cast<std::size_t>(maxSize), '\0');
    if (!CFStringGetCString(string, buffer.data(), maxSize, kCFStringEncodingUTF8)) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

double doubleValue(CFDictionaryRef dictionary, CFStringRef key) {
    auto value = dictionaryValue(dictionary, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return 0.0;
    }

    double result = 0.0;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &result);
    return result;
}

std::uint32_t uint32Value(CFDictionaryRef dictionary, CFStringRef key) {
    auto value = dictionaryValue(dictionary, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return 0;
    }

    std::uint32_t result = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &result);
    return result;
}

std::uint32_t firstUInt32InArray(CFDictionaryRef dictionary, CFStringRef key) {
    auto value = dictionaryValue(dictionary, key);
    if (value == nullptr) {
        return 0;
    }
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        std::uint32_t result = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &result);
        return result;
    }
    if (CFGetTypeID(value) != CFArrayGetTypeID()) {
        return 0;
    }

    auto array = static_cast<CFArrayRef>(value);
    if (CFArrayGetCount(array) == 0) {
        return 0;
    }
    auto first = CFArrayGetValueAtIndex(array, 0);
    if (first == nullptr || CFGetTypeID(first) != CFNumberGetTypeID()) {
        return 0;
    }

    std::uint32_t result = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(first), kCFNumberSInt32Type, &result);
    return result;
}

std::string whiteBalanceName(CFDictionaryRef exif) {
    auto value = dictionaryValue(exif, kCGImagePropertyExifWhiteBalance);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return {};
    }

    std::uint32_t whiteBalance = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &whiteBalance);
    if (whiteBalance == 0) {
        return "auto";
    }
    if (whiteBalance == 1) {
        return "manual";
    }
    return {};
}

std::string exposureBiasName(CFDictionaryRef exif) {
    auto value = dictionaryValue(exif, kCGImagePropertyExifExposureBiasValue);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return {};
    }

    double result = 0.0;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &result);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.2f", result);
    std::string text = buffer;
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text.empty() ? "0 EV" : text + " EV";
}

void populateImageIOMetadata(CFDictionaryRef properties, ImageMetadata& metadata) {
    metadata.colorModel = stringValue(properties, kCGImagePropertyColorModel);
    metadata.colorProfile = stringValue(properties, kCGImagePropertyProfileName);
    metadata.orientation = uint32Value(properties, kCGImagePropertyOrientation);
    if (metadata.orientation >= 5 && metadata.orientation <= 8) {
        std::swap(metadata.width, metadata.height);
    }

    if (auto tiff = dictionaryValueAsDictionary(properties, kCGImagePropertyTIFFDictionary)) {
        metadata.cameraMake = stringValue(tiff, kCGImagePropertyTIFFMake);
        metadata.cameraModel = stringValue(tiff, kCGImagePropertyTIFFModel);
        metadata.captureDate = stringValue(tiff, kCGImagePropertyTIFFDateTime);
    }
    if (auto exif = dictionaryValueAsDictionary(properties, kCGImagePropertyExifDictionary)) {
        metadata.exposureTimeSeconds = doubleValue(exif, kCGImagePropertyExifExposureTime);
        metadata.fNumber = doubleValue(exif, kCGImagePropertyExifFNumber);
        metadata.iso = firstUInt32InArray(exif, kCGImagePropertyExifISOSpeedRatings);
        metadata.focalLengthMM = doubleValue(exif, kCGImagePropertyExifFocalLength);
        metadata.lensModel = stringValue(exif, kCGImagePropertyExifLensModel);
        metadata.whiteBalance = whiteBalanceName(exif);
        metadata.exposureBias = exposureBiasName(exif);
        if (metadata.captureDate.empty()) {
            metadata.captureDate = stringValue(exif, kCGImagePropertyExifDateTimeOriginal);
        }
    }
}

InspectResult enrichWithImageIO(const std::filesystem::path& path, InspectResult result) {
    auto url = createFileUrl(path);
    if (url.get() == nullptr) {
        return result;
    }

    CFRef<CGImageSourceRef> source(CGImageSourceCreateWithURL(url.get(), nullptr));
    if (source.get() == nullptr || CGImageSourceGetCount(source.get()) == 0) {
        return result;
    }

    CFRef<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr));
    if (properties.get() != nullptr) {
        populateImageIOMetadata(properties.get(), result.metadata);
    }
    return result;
}

InspectResult inspectWithImageIO(const std::filesystem::path& path, ImageFormat format) {
    auto url = createFileUrl(path);
    if (url.get() == nullptr) {
        return error("InputPathInvalid", "Could not create file URL for input", path);
    }

    CFRef<CGImageSourceRef> source(CGImageSourceCreateWithURL(url.get(), nullptr));
    if (source.get() == nullptr || CGImageSourceGetCount(source.get()) == 0) {
        return error("InputFormatUnsupported", "ImageIO could not inspect the input image", path);
    }

    CFRef<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr));
    if (properties.get() == nullptr) {
        return error("ImageHeaderInvalid", "ImageIO did not return image properties", path);
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    CFNumberRef widthValue =
        static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelWidth));
    CFNumberRef heightValue =
        static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), kCGImagePropertyPixelHeight));
    if (widthValue != nullptr) {
        CFNumberGetValue(widthValue, kCFNumberSInt32Type, &width);
    }
    if (heightValue != nullptr) {
        CFNumberGetValue(heightValue, kCFNumberSInt32Type, &height);
    }
    if (width == 0 || height == 0) {
        return error("ImageHeaderInvalid", "ImageIO image dimensions were not found", path);
    }

    InspectResult result;
    result.ok = true;
    result.metadata.path = path.string();
    result.metadata.format = format;
    result.metadata.width = width;
    result.metadata.height = height;
    result.metadata.channels = 4;
    result.metadata.bitsPerChannel = 16;
    populateImageIOMetadata(properties.get(), result.metadata);
    if (format == ImageFormat::RAW) {
        result.metadata.rawDecoder = "Apple RAW / ImageIO";
    }
    return result;
}
#endif

} // namespace

std::string toString(ImageFormat format) {
    switch (format) {
    case ImageFormat::PNG:
        return "PNG";
    case ImageFormat::JPEG:
        return "JPEG";
    case ImageFormat::HEIF:
        return "HEIF";
    case ImageFormat::TIFF:
        return "TIFF";
    case ImageFormat::FITS:
        return "FITS";
    case ImageFormat::RAW:
        return "RAW";
    case ImageFormat::Unknown:
    default:
        return "UNKNOWN";
    }
}

InspectResult ImageInspector::inspect(const std::filesystem::path& path) const {
    if (!std::filesystem::exists(path)) {
        return error("InputFileNotFound", "Input file does not exist", path);
    }
    if (!std::filesystem::is_regular_file(path)) {
        return error("InputPathInvalid", "Input path is not a regular file", path);
    }

    if (isRawPath(path)) {
#ifdef __APPLE__
        return inspectWithImageIO(path, ImageFormat::RAW);
#else
        return error("PlatformUnsupported", "RAW inspection currently uses Apple ImageIO", path);
#endif
    }

    if (isFitsPath(path)) {
        const FitsCodec fits;
        return fits.inspect(path);
    }

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize == 0) {
        return error("InputReadFailed", "Input file could not be read or is empty", path);
    }

    const auto extension = path.extension().string();
    std::string lowerExtension = extension;
    std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerExtension == ".heic" || lowerExtension == ".heif" || lowerExtension == ".hif") {
#ifdef __APPLE__
        return inspectWithImageIO(path, ImageFormat::HEIF);
#else
        return error("PlatformUnsupported", "HEIF inspection currently uses Apple ImageIO", path);
#endif
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return error("InputReadFailed", "Input file could not be read or is empty", path);
    }
    std::array<unsigned char, 8> prefix{};
    const auto prefixSize = static_cast<std::size_t>(std::min<std::uintmax_t>(fileSize, prefix.size()));
    if (!readAt(file, fileSize, 0, prefix.data(), prefixSize)) {
        return error("InputReadFailed", "Input file could not be read or is empty", path);
    }

    if (prefixSize >= 8 && prefix[0] == 0x89 && prefix[1] == 'P' && prefix[2] == 'N' && prefix[3] == 'G') {
        auto result = inspectPng(file, fileSize, path);
#ifdef __APPLE__
        if (result.ok) {
            return enrichWithImageIO(path, std::move(result));
        }
#endif
        return result;
    }
    if (prefixSize >= 2 && prefix[0] == 0xFF && prefix[1] == 0xD8) {
        auto result = inspectJpeg(file, fileSize, path);
#ifdef __APPLE__
        if (result.ok) {
            return enrichWithImageIO(path, std::move(result));
        }
#endif
        return result;
    }
    if (prefixSize >= 4 && ((prefix[0] == 'I' && prefix[1] == 'I') ||
                            (prefix[0] == 'M' && prefix[1] == 'M'))) {
        auto result = inspectTiff(file, fileSize, path);
        if (result.ok && isRawPath(path)) {
            result.metadata.format = ImageFormat::RAW;
        }
#ifdef __APPLE__
        if (result.ok) {
            return enrichWithImageIO(path, std::move(result));
        }
#endif
        return result;
    }

    return error("InputFormatUnsupported", "Supported formats are PNG, JPEG, TIFF, FITS, and Apple RAW formats", path);
}

} // namespace photonstack
