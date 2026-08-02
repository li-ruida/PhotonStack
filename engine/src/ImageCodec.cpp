#include "photonstack/ImageCodec.hpp"

#ifdef __APPLE__
#include "AppleRawDecoder.hpp"
#endif

#include "photonstack/FitsCodec.hpp"
#include "photonstack/RawCodec.hpp"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace photonstack {
namespace {

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

std::string lowercaseExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

float finiteUnitValue(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}

std::uint8_t floatToByte(float value) {
    const float clamped = finiteUnitValue(value);
    return static_cast<std::uint8_t>(clamped * 255.0F + 0.5F);
}

std::uint16_t floatToUInt16(float value) {
    const float clamped = finiteUnitValue(value);
    return static_cast<std::uint16_t>(clamped * 65535.0F + 0.5F);
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
    [[nodiscard]] T release() {
        T value = value_;
        value_ = nullptr;
        return value;
    }

  private:
    T value_;
};

bool isJpegPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".jpg" || extension == ".jpeg";
}

bool isHeifPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".heic" || extension == ".heif" || extension == ".hif";
}

bool isRasterOutputPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".heic" ||
           extension == ".heif" || extension == ".hif" || extension == ".tif" || extension == ".tiff";
}

bool validRawDecodeOptionsImpl(const RawDecodeOptions& options) {
    const auto validWhiteBalance = [&] {
        switch (options.whiteBalanceMode) {
        case RawWhiteBalanceMode::Camera:
        case RawWhiteBalanceMode::Auto:
        case RawWhiteBalanceMode::Daylight:
        case RawWhiteBalanceMode::Manual:
            return true;
        }
        return false;
    }();
    const auto validBlackLevel = [&] {
        switch (options.blackLevelMode) {
        case RawBlackLevelMode::Camera:
        case RawBlackLevelMode::Auto:
        case RawBlackLevelMode::Manual:
            return true;
        }
        return false;
    }();
    const auto validDemosaic = [&] {
        switch (options.demosaicQuality) {
        case RawDemosaicQuality::Fast:
        case RawDemosaicQuality::Balanced:
        case RawDemosaicQuality::High:
            return true;
        }
        return false;
    }();
    return validWhiteBalance && validBlackLevel && validDemosaic &&
           std::isfinite(options.manualWhiteBalanceTemperature) &&
           options.manualWhiteBalanceTemperature >= 2000.0F &&
           options.manualWhiteBalanceTemperature <= 50000.0F &&
           std::isfinite(options.manualWhiteBalanceTint) &&
           options.manualWhiteBalanceTint >= -150.0F && options.manualWhiteBalanceTint <= 150.0F &&
           std::isfinite(options.exposureBias) && options.exposureBias >= -5.0F && options.exposureBias <= 5.0F &&
           std::isfinite(options.manualBlackLevel) &&
           options.manualBlackLevel >= 0.0F && options.manualBlackLevel <= 1.0F;
}

bool validFitsDecodeOptions(const FitsDecodeOptions& options) {
    switch (options.mode) {
    case FitsDecodeMode::DisplayNormalized:
    case FitsDecodeMode::Scientific:
        return true;
    }
    return false;
}

bool validImageWriteOptions(const std::filesystem::path& path, const ImageWriteOptions& options) {
    const auto validBitDepth = [&] {
        switch (options.bitDepth) {
        case ImageWriteBitDepth::Auto:
        case ImageWriteBitDepth::Eight:
        case ImageWriteBitDepth::Sixteen:
            return true;
        }
        return false;
    }();
    const auto validColorSpace = [&] {
        switch (options.colorSpace) {
        case ImageWriteColorSpace::SRGB:
        case ImageWriteColorSpace::LinearSRGB:
            return true;
        }
        return false;
    }();
    const bool validQuality = (!isJpegPath(path) && !isHeifPath(path)) ||
                              (std::isfinite(options.quality) && options.quality >= 0.0F &&
                               options.quality <= 1.0F);
    return validBitDepth && validColorSpace && validQuality;
}

CFStringRef outputTypeForPath(const std::filesystem::path& path) {
    const auto extension = lowercaseExtension(path);
    if (extension == ".jpg" || extension == ".jpeg") {
        return CFSTR("public.jpeg");
    }
    if (extension == ".tif" || extension == ".tiff") {
        return CFSTR("public.tiff");
    }
    if (extension == ".heic" || extension == ".heif" || extension == ".hif") {
        return CFSTR("public.heic");
    }
    if (extension == ".png") {
        return CFSTR("public.png");
    }
    return nullptr;
}

bool shouldWrite16Bit(const std::filesystem::path& path, const ImageWriteOptions& options) {
    return effectiveImageWriteBitDepth(path, options) == ImageWriteBitDepth::Sixteen;
}

CFRef<CGColorSpaceRef> createOutputColorSpace(const ImageWriteOptions& options) {
    switch (options.colorSpace) {
    case ImageWriteColorSpace::LinearSRGB:
        return CFRef<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB));
    case ImageWriteColorSpace::SRGB:
        return CFRef<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    }
    return CFRef<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
}

CFRef<CFDictionaryRef> createDestinationProperties(const std::filesystem::path& path, const ImageWriteOptions& options) {
    if (!isJpegPath(path) && !isHeifPath(path)) {
        return CFRef<CFDictionaryRef>();
    }

    const float quality = std::clamp(options.quality, 0.0F, 1.0F);
    CFRef<CFNumberRef> qualityNumber(CFNumberCreate(nullptr, kCFNumberFloatType, &quality));
    if (qualityNumber.get() == nullptr) {
        return CFRef<CFDictionaryRef>();
    }

    CFRef<CFMutableDictionaryRef> properties(
        CFDictionaryCreateMutable(nullptr, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (properties.get() == nullptr) {
        return CFRef<CFDictionaryRef>();
    }
    CFDictionarySetValue(properties.get(), kCGImageDestinationLossyCompressionQuality, qualityNumber.get());
    return CFRef<CFDictionaryRef>(properties.release());
}

CFRef<CFURLRef> createFileUrl(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).string();
    CFRef<CFStringRef> cfPath(CFStringCreateWithCString(nullptr, absolute.c_str(), kCFStringEncodingUTF8));
    if (cfPath.get() == nullptr) {
        return CFRef<CFURLRef>();
    }
    return CFRef<CFURLRef>(CFURLCreateWithFileSystemPath(nullptr, cfPath.get(), kCFURLPOSIXPathStyle, false));
}

CFRef<CFDictionaryRef> createImageSourceDecodeOptions() {
    const void* keys[] = {
        kCGImageSourceShouldCache,
        kCGImageSourceShouldCacheImmediately,
    };
    const void* values[] = {
        kCFBooleanTrue,
        kCFBooleanTrue,
    };
    return CFRef<CFDictionaryRef>(CFDictionaryCreate(
        nullptr,
        keys,
        values,
        2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    ));
}

CFRef<CFDictionaryRef> createTransformedImageDecodeOptions(std::uint32_t maxPixelSize) {
    const auto clampedSize = std::min<std::uint32_t>(
        std::max<std::uint32_t>(maxPixelSize, 1),
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())
    );
    const int size = static_cast<int>(clampedSize);
    CFRef<CFNumberRef> sizeNumber(CFNumberCreate(nullptr, kCFNumberIntType, &size));
    if (sizeNumber.get() == nullptr) {
        return CFRef<CFDictionaryRef>();
    }

    const void* keys[] = {
        kCGImageSourceCreateThumbnailFromImageAlways,
        kCGImageSourceCreateThumbnailWithTransform,
        kCGImageSourceShouldCacheImmediately,
        kCGImageSourceThumbnailMaxPixelSize,
    };
    const void* values[] = {
        kCFBooleanTrue,
        kCFBooleanTrue,
        kCFBooleanTrue,
        sizeNumber.get(),
    };
    return CFRef<CFDictionaryRef>(CFDictionaryCreate(
        nullptr,
        keys,
        values,
        4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    ));
}

struct ImageSourceGeometry {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t orientation = 1;
};

ImageSourceGeometry imageSourceGeometry(CGImageSourceRef source) {
    ImageSourceGeometry geometry;
    CFRef<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
    if (properties.get() == nullptr) {
        return geometry;
    }

    const auto readNumber = [&](CFStringRef key, std::uint32_t& value) {
        auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(properties.get(), key));
        if (number != nullptr && CFGetTypeID(number) == CFNumberGetTypeID()) {
            std::int64_t rawValue = 0;
            if (CFNumberGetValue(number, kCFNumberSInt64Type, &rawValue) && rawValue > 0 &&
                rawValue <= std::numeric_limits<std::uint32_t>::max()) {
                value = static_cast<std::uint32_t>(rawValue);
            }
        }
    };
    readNumber(kCGImagePropertyPixelWidth, geometry.width);
    readNumber(kCGImagePropertyPixelHeight, geometry.height);
    readNumber(kCGImagePropertyOrientation, geometry.orientation);
    if (geometry.orientation < 1 || geometry.orientation > 8) {
        geometry.orientation = 1;
    }
    return geometry;
}

CFRef<CGColorSpaceRef> createDecodeColorSpace() {
    return CFRef<CGColorSpaceRef>(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
}

float srgbToLinear(float value);

float linearToSRGB(float value) {
    const float clamped = finiteUnitValue(value);
    if (clamped <= 0.0031308F) {
        return clamped * 12.92F;
    }
    return 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

float colorComponentForOutput(
    float value,
    ColorEncoding sourceEncoding,
    ImageWriteColorSpace outputColorSpace
) {
    if (sourceEncoding == ColorEncoding::SRGB && outputColorSpace == ImageWriteColorSpace::LinearSRGB) {
        return srgbToLinear(value);
    }
    if (sourceEncoding == ColorEncoding::Linear && outputColorSpace == ImageWriteColorSpace::SRGB) {
        return linearToSRGB(value);
    }
    return finiteUnitValue(value);
}

float colorComponentFlattenedToBlack(
    float value,
    float alpha,
    ColorEncoding sourceEncoding,
    ImageWriteColorSpace outputColorSpace
) {
    if (sourceEncoding == ColorEncoding::Unknown) {
        return colorComponentForOutput(value, sourceEncoding, outputColorSpace) * alpha;
    }
    const float linear = sourceEncoding == ColorEncoding::SRGB
                             ? srgbToLinear(value)
                             : finiteUnitValue(value);
    const float composited = linear * alpha;
    return outputColorSpace == ImageWriteColorSpace::SRGB ? linearToSRGB(composited) : composited;
}

std::vector<std::uint8_t> createRgba8(
    const ImageBuffer& image,
    ImageWriteColorSpace outputColorSpace,
    bool flattenToBlack
) {
    std::vector<std::uint8_t> rgba(image.sampleCount(), 0);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        const float alpha = finiteUnitValue(image.pixels[offset + 3]);
        const auto component = [&](std::size_t channel) {
            return flattenToBlack
                       ? colorComponentFlattenedToBlack(
                             image.pixels[offset + channel], alpha, image.colorEncoding, outputColorSpace
                         )
                       : colorComponentForOutput(
                             image.pixels[offset + channel], image.colorEncoding, outputColorSpace
                         ) * alpha;
        };
        rgba[offset] = floatToByte(component(0));
        rgba[offset + 1] = floatToByte(component(1));
        rgba[offset + 2] = floatToByte(component(2));
        rgba[offset + 3] = floatToByte(flattenToBlack ? 1.0F : alpha);
    }
    return rgba;
}

std::vector<std::uint8_t> createRgba16BigEndian(
    const ImageBuffer& image,
    ImageWriteColorSpace outputColorSpace
) {
    std::vector<std::uint8_t> rgba(image.sampleCount() * 2, 0);
    const auto store = [&](std::size_t sample, float value) {
        const auto encoded = floatToUInt16(value);
        rgba[sample * 2] = static_cast<std::uint8_t>((encoded >> 8U) & 0xFFU);
        rgba[sample * 2 + 1] = static_cast<std::uint8_t>(encoded & 0xFFU);
    };
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        const float alpha = finiteUnitValue(image.pixels[offset + 3]);
        store(offset, colorComponentForOutput(image.pixels[offset], image.colorEncoding, outputColorSpace) * alpha);
        store(offset + 1,
              colorComponentForOutput(image.pixels[offset + 1], image.colorEncoding, outputColorSpace) * alpha);
        store(offset + 2,
              colorComponentForOutput(image.pixels[offset + 2], image.colorEncoding, outputColorSpace) * alpha);
        store(offset + 3, alpha);
    }
    return rgba;
}

struct TemporaryImageRowCache {
    std::filesystem::path directory;

    TemporaryImageRowCache() = default;
    TemporaryImageRowCache(const TemporaryImageRowCache&) = delete;
    TemporaryImageRowCache& operator=(const TemporaryImageRowCache&) = delete;

    ~TemporaryImageRowCache() {
        if (directory.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    [[nodiscard]] std::filesystem::path pixelPath() const { return directory / "pixels.rgba"; }
};

ImageWriteResult createTemporaryImageRowCache(TemporaryImageRowCache& cache, std::uintmax_t requiredBytes) {
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error) {
        return writeError("TemporaryStorageUnavailable", "Unable to find the temporary directory: " + error.message());
    }

    const auto space = std::filesystem::space(root, error);
    if (error) {
        return writeError("TemporaryStorageUnavailable", "Unable to inspect temporary storage: " + error.message());
    }
    constexpr std::uintmax_t minimumReserve = 64ULL * 1024ULL * 1024ULL;
    if (space.available < requiredBytes || space.available - requiredBytes < minimumReserve) {
        return writeError("TemporaryStorageInsufficient",
                          "Image row cache requires " + std::to_string(requiredBytes) +
                              " temporary bytes, but only " + std::to_string(space.available) + " are available");
    }

    static std::atomic<std::uint64_t> sequence = 0;
    const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    for (std::size_t attempt = 0; attempt < 64; ++attempt) {
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = root / ("photonstack-image-row-cache-" + std::to_string(timestamp) + "-" +
                                       std::to_string(id));
        error.clear();
        if (std::filesystem::create_directory(candidate, error)) {
            std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::error_code cleanupError;
                std::filesystem::remove_all(candidate, cleanupError);
                return writeError("TemporaryStorageUnavailable",
                                  "Unable to secure the image row cache: " + error.message());
            }
            cache.directory = candidate;
            ImageWriteResult result;
            result.ok = true;
            return result;
        }
        if (error) {
            return writeError("TemporaryStorageUnavailable",
                              "Unable to create the image row cache: " + error.message());
        }
    }
    return writeError("TemporaryStorageUnavailable", "Unable to allocate a unique image row cache directory");
}
#endif

template <typename Callback> void parallelForPixels(const ImageBuffer& image, Callback callback) {
    const std::size_t pixels = image.pixelCount();
    if (pixels < 262144) {
        callback(0, pixels);
        return;
    }

    const unsigned int workerCount = std::max(1U, std::min<unsigned int>(std::thread::hardware_concurrency(), 8U));
    const std::size_t blockSize = (pixels + workerCount - 1) / workerCount;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned int worker = 0; worker < workerCount; ++worker) {
        const std::size_t begin = std::min<std::size_t>(pixels, static_cast<std::size_t>(worker) * blockSize);
        const std::size_t end = std::min<std::size_t>(pixels, begin + blockSize);
        if (begin >= end) {
            break;
        }
        workers.emplace_back([=, &callback]() {
            callback(begin, end);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
}

float pixelCoverage(const ImageBuffer& image, std::size_t offset) {
    if (image.channels != 4) {
        return 1.0F;
    }
    const float alpha = image.pixels[offset + 3];
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0F, 1.0F) : 0.0F;
}

float scaledColorValue(float value, float gain) {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    const double product = std::max(0.0, static_cast<double>(value) * static_cast<double>(gain));
    return static_cast<float>(
        std::min(product, static_cast<double>(std::numeric_limits<float>::max()))
    );
}

std::array<float, 3> automaticBlackLevel(const ImageBuffer& image) {
    constexpr std::size_t binCount = 4096;
    constexpr double percentile = 0.001;
    std::array<std::array<double, binCount>, 3> histograms{};
    double totalWeight = 0.0;
    for (std::size_t offset = 0; offset + 2 < image.pixels.size(); offset += image.channels) {
        const double coverage = pixelCoverage(image, offset);
        if (!(coverage > 0.0)) {
            continue;
        }
        bool valid = true;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            valid = valid && std::isfinite(image.pixels[offset + channel]);
        }
        if (!valid) {
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double value = std::clamp(static_cast<double>(image.pixels[offset + channel]), 0.0, 1.0);
            const auto bin = std::min<std::size_t>(
                binCount - 1,
                static_cast<std::size_t>(value * static_cast<double>(binCount - 1))
            );
            histograms[channel][bin] += coverage;
        }
        totalWeight += coverage;
    }

    std::array<float, 3> levels{};
    if (!(totalWeight > 0.0)) {
        return levels;
    }
    const double targetWeight = totalWeight * percentile;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        double cumulative = 0.0;
        for (std::size_t bin = 0; bin < binCount; ++bin) {
            cumulative += histograms[channel][bin];
            if (cumulative >= targetWeight) {
                levels[channel] = static_cast<float>(bin) / static_cast<float>(binCount - 1);
                break;
            }
        }
    }
    return levels;
}

void applyBlackLevel(ImageBuffer& image, const RawDecodeOptions& options) {
    if (image.channels < 3 || options.blackLevelMode == RawBlackLevelMode::Camera) {
        return;
    }

    const auto levels = options.blackLevelMode == RawBlackLevelMode::Manual
                            ? std::array<float, 3>{options.manualBlackLevel, options.manualBlackLevel,
                                                   options.manualBlackLevel}
                            : automaticBlackLevel(image);

    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            if (!(pixelCoverage(image, offset) > 0.0F)) {
                image.pixels[offset] = 0.0F;
                image.pixels[offset + 1] = 0.0F;
                image.pixels[offset + 2] = 0.0F;
                continue;
            }
            image.pixels[offset] = std::max(0.0F, image.pixels[offset] - levels[0]);
            image.pixels[offset + 1] = std::max(0.0F, image.pixels[offset + 1] - levels[1]);
            image.pixels[offset + 2] = std::max(0.0F, image.pixels[offset + 2] - levels[2]);
        }
    });
}

std::array<double, 3> correlatedColorTemperatureRGB(double temperature) {
    const double t = std::clamp(temperature, 1667.0, 50000.0);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double x = t <= 4000.0
                         ? -0.2661239e9 / t3 - 0.2343580e6 / t2 + 0.8776956e3 / t + 0.179910
                         : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    double y = 0.0;
    if (t <= 2222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }
    const double safeY = std::max(y, 1.0e-6);
    const double X = x / safeY;
    const double Y = 1.0;
    const double Z = std::max(0.0, (1.0 - x - y) / safeY);
    return {
        std::max(1.0e-6, 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z),
        std::max(1.0e-6, -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z),
        std::max(1.0e-6, 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z),
    };
}

std::array<float, 3> manualWhiteBalanceGains(float temperature, float tint) {
    const auto reference = correlatedColorTemperatureRGB(6500.0);
    const auto source = correlatedColorTemperatureRGB(temperature);
    std::array<double, 3> gains{
        reference[0] / source[0],
        reference[1] / source[1],
        reference[2] / source[2],
    };
    gains[1] *= std::exp2(-static_cast<double>(tint) / 150.0);
    const double normalization = std::max(gains[1], 1.0e-6);
    return {
        static_cast<float>(gains[0] / normalization),
        1.0F,
        static_cast<float>(gains[2] / normalization),
    };
}

void applyWhiteBalance(ImageBuffer& image, const RawDecodeOptions& options) {
    if (image.channels < 3 || options.whiteBalanceMode == RawWhiteBalanceMode::Camera) {
        return;
    }

    float gains[3] = {1.0F, 1.0F, 1.0F};
    if (options.whiteBalanceMode == RawWhiteBalanceMode::Daylight ||
        options.whiteBalanceMode == RawWhiteBalanceMode::Manual) {
        const auto manual = manualWhiteBalanceGains(
            options.whiteBalanceMode == RawWhiteBalanceMode::Daylight
                ? 5500.0F
                : options.manualWhiteBalanceTemperature,
            options.whiteBalanceMode == RawWhiteBalanceMode::Daylight ? 0.0F : options.manualWhiteBalanceTint
        );
        gains[0] = manual[0];
        gains[1] = manual[1];
        gains[2] = manual[2];
    } else if (options.whiteBalanceMode == RawWhiteBalanceMode::Auto) {
        double sum[3] = {0.0, 0.0, 0.0};
        double totalWeight = 0.0;
        for (std::size_t offset = 0; offset + 2 < image.pixels.size(); offset += image.channels) {
            const double coverage = pixelCoverage(image, offset);
            if (!(coverage > 0.0) || !std::isfinite(image.pixels[offset]) ||
                !std::isfinite(image.pixels[offset + 1]) || !std::isfinite(image.pixels[offset + 2])) {
                continue;
            }
            sum[0] += static_cast<double>(image.pixels[offset]) * coverage;
            sum[1] += static_cast<double>(image.pixels[offset + 1]) * coverage;
            sum[2] += static_cast<double>(image.pixels[offset + 2]) * coverage;
            totalWeight += coverage;
        }
        if (totalWeight > 0.0) {
            const double meanR = sum[0] / totalWeight;
            const double meanG = sum[1] / totalWeight;
            const double meanB = sum[2] / totalWeight;
            const double target = (meanR + meanG + meanB) / 3.0;
            gains[0] = meanR > 0.000001 ? static_cast<float>(target / meanR) : 1.0F;
            gains[1] = meanG > 0.000001 ? static_cast<float>(target / meanG) : 1.0F;
            gains[2] = meanB > 0.000001 ? static_cast<float>(target / meanB) : 1.0F;
        }
    }

    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            if (!(pixelCoverage(image, offset) > 0.0F)) {
                image.pixels[offset] = 0.0F;
                image.pixels[offset + 1] = 0.0F;
                image.pixels[offset + 2] = 0.0F;
                continue;
            }
            image.pixels[offset] = scaledColorValue(image.pixels[offset], gains[0]);
            image.pixels[offset + 1] = scaledColorValue(image.pixels[offset + 1], gains[1]);
            image.pixels[offset + 2] = scaledColorValue(image.pixels[offset + 2], gains[2]);
        }
    });
}

void applyExposureBias(ImageBuffer& image, float exposureBias) {
    if (!std::isfinite(exposureBias) || std::abs(exposureBias) < 0.0001F || image.channels < 3) {
        return;
    }

    const double multiplier = std::exp2(static_cast<double>(exposureBias));
    const auto scaledValue = [multiplier](float value) {
        const double scaled = std::max(0.0, static_cast<double>(value) * multiplier);
        return static_cast<float>(std::min(scaled, static_cast<double>(std::numeric_limits<float>::max())));
    };
    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            if (!(pixelCoverage(image, offset) > 0.0F)) {
                image.pixels[offset] = 0.0F;
                image.pixels[offset + 1] = 0.0F;
                image.pixels[offset + 2] = 0.0F;
                continue;
            }
            image.pixels[offset] = scaledValue(image.pixels[offset]);
            image.pixels[offset + 1] = scaledValue(image.pixels[offset + 1]);
            image.pixels[offset + 2] = scaledValue(image.pixels[offset + 2]);
        }
    });
}

float srgbToLinear(float value) {
    const float clamped = finiteUnitValue(value);
    if (clamped <= 0.04045F) {
        return clamped / 12.92F;
    }
    return std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

void convertSRGBToLinear(ImageBuffer& image) {
    if (image.channels < 3 || image.colorEncoding == ColorEncoding::Linear) {
        return;
    }

    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            image.pixels[offset] = srgbToLinear(image.pixels[offset]);
            image.pixels[offset + 1] = srgbToLinear(image.pixels[offset + 1]);
            image.pixels[offset + 2] = srgbToLinear(image.pixels[offset + 2]);
        }
    });
    image.colorEncoding = ColorEncoding::Linear;
}

float linearToSRGBForRawOptions(float value) {
    const float clamped = finiteUnitValue(value);
    if (clamped <= 0.0031308F) {
        return clamped * 12.92F;
    }
    return 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

void convertLinearToSRGB(ImageBuffer& image) {
    if (image.channels < 3 || image.colorEncoding == ColorEncoding::SRGB) {
        return;
    }
    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            image.pixels[offset] = linearToSRGBForRawOptions(image.pixels[offset]);
            image.pixels[offset + 1] = linearToSRGBForRawOptions(image.pixels[offset + 1]);
            image.pixels[offset + 2] = linearToSRGBForRawOptions(image.pixels[offset + 2]);
        }
    });
    image.colorEncoding = ColorEncoding::SRGB;
}

void clearHiddenRawColor(ImageBuffer& image) {
    if (image.channels != 4) {
        return;
    }
    parallelForPixels(image, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pixel = begin; pixel < end; ++pixel) {
            const auto offset = pixel * image.channels;
            float& alpha = image.pixels[offset + 3];
            if (!std::isfinite(alpha) || alpha <= 0.0F) {
                image.pixels[offset] = 0.0F;
                image.pixels[offset + 1] = 0.0F;
                image.pixels[offset + 2] = 0.0F;
                alpha = 0.0F;
            } else {
                alpha = std::min(alpha, 1.0F);
            }
        }
    });
}

void applyRawDecodeOptionsImpl(ImageBuffer& image, const RawDecodeOptions& options) {
    clearHiddenRawColor(image);
    if (image.colorEncoding == ColorEncoding::SRGB) {
        convertSRGBToLinear(image);
    }
    applyBlackLevel(image, options);
    applyWhiteBalance(image, options);
    applyExposureBias(image, options.exposureBias);
    if (options.linearOutput) {
        image.colorEncoding = ColorEncoding::Linear;
    } else {
        convertLinearToSRGB(image);
    }
}

} // namespace

const char* imageReadBackendName(ImageReadBackend backend) {
    switch (backend) {
    case ImageReadBackend::ImageIO:
        return "imageio";
    case ImageReadBackend::AppleRaw:
        return "apple-raw";
    case ImageReadBackend::Fits:
        return "fits";
    case ImageReadBackend::Unknown:
        return "unknown";
    }
    return "unknown";
}

bool isValidRawDecodeOptions(const RawDecodeOptions& options) {
    return validRawDecodeOptionsImpl(options);
}

void applyRawDecodeOptions(ImageBuffer& image, const RawDecodeOptions& options) {
    if (!isValidRawDecodeOptions(options) || image.empty() ||
        (image.channels != 3 && image.channels != 4) || image.pixels.size() != image.sampleCount()) {
        return;
    }
    applyRawDecodeOptionsImpl(image, options);
}

bool isSupportedImageWritePath(const std::filesystem::path& path) {
    return isFitsPath(path) || isRasterOutputPath(path);
}

ImageWriteBitDepth effectiveImageWriteBitDepth(
    const std::filesystem::path& path,
    const ImageWriteOptions& options
) {
    if (!isRasterOutputPath(path)) {
        return ImageWriteBitDepth::Auto;
    }
    const auto extension = lowercaseExtension(path);
    if (extension == ".jpg" || extension == ".jpeg" || extension == ".heic" || extension == ".heif" ||
        extension == ".hif") {
        return ImageWriteBitDepth::Eight;
    }
    switch (options.bitDepth) {
    case ImageWriteBitDepth::Eight:
        return ImageWriteBitDepth::Eight;
    case ImageWriteBitDepth::Sixteen:
        return ImageWriteBitDepth::Sixteen;
    case ImageWriteBitDepth::Auto:
        return extension == ".tif" || extension == ".tiff" ? ImageWriteBitDepth::Sixteen
                                                             : ImageWriteBitDepth::Eight;
    }
    return ImageWriteBitDepth::Eight;
}

ImageReadResult ImageCodec::read(const std::filesystem::path& path) const {
    return read(path, {});
}

ImageReadResult ImageCodec::read(const std::filesystem::path& path, const ImageReadOptions& options) const {
    if (isFitsPath(path)) {
        if (!validFitsDecodeOptions(options.fits)) {
            return readError("ArgumentInvalid", "FITS decode options are invalid");
        }
        const FitsCodec fits;
        return fits.read(path, options.fits);
    }
    if (isRawPath(path) && !isValidRawDecodeOptions(options.raw)) {
        return readError("ArgumentInvalid", "RAW decode options are invalid");
    }

#ifndef __APPLE__
    (void)path;
    (void)options;
    return readError("PlatformUnsupported", "ImageCodec currently uses Apple ImageIO");
#else
    if (!std::filesystem::exists(path)) {
        return readError("InputFileNotFound", "Input file does not exist");
    }

    auto url = createFileUrl(path);
    if (url.get() == nullptr) {
        return readError("InputPathInvalid", "Could not create file URL for input");
    }

    CFRef<CGImageSourceRef> source(CGImageSourceCreateWithURL(url.get(), nullptr));
    if (source.get() == nullptr || CGImageSourceGetCount(source.get()) == 0) {
        return readError("InputFormatUnsupported", "ImageIO could not read the input image");
    }

    const bool rawInput = isRawPath(path);
    const bool proxyDecode = options.maximumDecodePixelSize > 0;
    const auto sourceGeometry = imageSourceGeometry(source.get());
    std::string rawFallbackErrorCode;
    std::string rawFallbackMessage;
    if (rawInput && !proxyDecode) {
        auto rawResult = decodeAppleRawFloat(path, options.raw);
        if (rawResult.ok) {
            auto postDecodeOptions = options.raw;
            postDecodeOptions.exposureBias = 0.0F;
            if (postDecodeOptions.whiteBalanceMode == RawWhiteBalanceMode::Daylight ||
                postDecodeOptions.whiteBalanceMode == RawWhiteBalanceMode::Manual) {
                postDecodeOptions.whiteBalanceMode = RawWhiteBalanceMode::Camera;
            }
            applyRawDecodeOptions(rawResult.image, postDecodeOptions);
            return rawResult;
        }
        rawFallbackErrorCode = std::move(rawResult.errorCode);
        rawFallbackMessage = std::move(rawResult.message);
    }

    CGImageRef decodedImage = nullptr;
    if (proxyDecode) {
        auto decodeOptions = createTransformedImageDecodeOptions(options.maximumDecodePixelSize);
        decodedImage = CGImageSourceCreateThumbnailAtIndex(source.get(), 0, decodeOptions.get());
    } else if (rawInput) {
        auto decodeOptions = createTransformedImageDecodeOptions(
            std::max(sourceGeometry.width, sourceGeometry.height)
        );
        decodedImage = CGImageSourceCreateThumbnailAtIndex(source.get(), 0, decodeOptions.get());
    } else if (sourceGeometry.orientation != 1 && sourceGeometry.width > 0 && sourceGeometry.height > 0) {
        auto decodeOptions = createTransformedImageDecodeOptions(
            std::max(sourceGeometry.width, sourceGeometry.height)
        );
        decodedImage = CGImageSourceCreateThumbnailAtIndex(source.get(), 0, decodeOptions.get());
    } else {
        auto decodeOptions = createImageSourceDecodeOptions();
        decodedImage = CGImageSourceCreateImageAtIndex(source.get(), 0, decodeOptions.get());
    }
    CFRef<CGImageRef> image(decodedImage);
    if (image.get() == nullptr) {
        return readError("ImageDecodeFailed", "ImageIO failed to decode the input image");
    }

    const auto width = CGImageGetWidth(image.get());
    const auto height = CGImageGetHeight(image.get());
    if (width == 0 || height == 0 || width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max() ||
        width > std::numeric_limits<std::size_t>::max() / 8 ||
        height > std::numeric_limits<std::size_t>::max() / (width * 8)) {
        return readError("ImageDecodeFailed", "Decoded image has invalid dimensions");
    }

    const auto rowBytes = width * 8;
    std::vector<std::uint8_t> rgba;
    try {
        rgba.assign(rowBytes * height, 0);
    } catch (const std::bad_alloc&) {
        return readError("ImageAllocationFailed", "Could not allocate the decoded image buffer");
    }
    CFRef<CGColorSpaceRef> colorSpace(createDecodeColorSpace());
    if (colorSpace.get() == nullptr) {
        return readError("ImageDecodeFailed", "Could not create sRGB color space");
    }

    CFRef<CGContextRef> context(
        CGBitmapContextCreate(rgba.data(), width, height, 16, rowBytes, colorSpace.get(),
                              static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder16Big));
    if (context.get() == nullptr) {
        return readError("ImageDecodeFailed", "Could not create decode bitmap context");
    }

    CGContextDrawImage(context.get(), CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)),
                       image.get());

    ImageReadResult result;
    result.ok = true;
    result.backend = ImageReadBackend::ImageIO;
    if (rawInput && !proxyDecode) {
        result.usedFallback = true;
        result.fallbackErrorCode = std::move(rawFallbackErrorCode);
        result.fallbackMessage = std::move(rawFallbackMessage);
    }
    result.image.width = static_cast<std::uint32_t>(width);
    result.image.height = static_cast<std::uint32_t>(height);
    result.image.channels = 4;
    result.image.format = PixelFormat::Float32RGBA;
    result.image.colorEncoding = ColorEncoding::SRGB;
    result.image.sourceBitsPerChannel = static_cast<std::uint16_t>(
        std::clamp<std::size_t>(CGImageGetBitsPerComponent(image.get()), 1, 16)
    );
    try {
        result.image.pixels.resize(result.image.sampleCount());
    } catch (const std::bad_alloc&) {
        return readError("ImageAllocationFailed", "Could not allocate the Float32 image buffer");
    }
    for (std::size_t i = 0; i < result.image.pixels.size(); ++i) {
        const auto value =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(rgba[i * 2]) << 8U) | rgba[i * 2 + 1]);
        result.image.pixels[i] = static_cast<float>(value) / 65535.0F;
    }
    for (std::size_t pixel = 0; pixel < result.image.pixelCount(); ++pixel) {
        const auto offset = pixel * result.image.channels;
        const float alpha = result.image.pixels[offset + 3];
        if (alpha > 1.0e-6F) {
            result.image.pixels[offset] = std::clamp(result.image.pixels[offset] / alpha, 0.0F, 1.0F);
            result.image.pixels[offset + 1] = std::clamp(result.image.pixels[offset + 1] / alpha, 0.0F, 1.0F);
            result.image.pixels[offset + 2] = std::clamp(result.image.pixels[offset + 2] / alpha, 0.0F, 1.0F);
        }
    }
    if (rawInput) {
        applyRawDecodeOptions(result.image, options.raw);
    }
    return result;
#endif
}

ImageWriteResult ImageCodec::write(const ImageBuffer& image, const std::filesystem::path& path) const {
    ImageWriteOptions options;
    if (image.colorEncoding == ColorEncoding::Linear && image.sourceBitsPerChannel <= 16) {
        options.colorSpace = ImageWriteColorSpace::LinearSRGB;
    }
    return write(image, path, options);
}

ImageWriteResult ImageCodec::write(const ImageBuffer& image, const std::filesystem::path& path, const ImageWriteOptions& options) const {
    if (!validImageWriteOptions(path, options)) {
        return writeError("ArgumentInvalid", "Image write options are invalid");
    }
    if (isFitsPath(path)) {
        const FitsCodec fits;
        return fits.write(image, path);
    }
    if (!isRasterOutputPath(path)) {
        return writeError("OutputFormatUnsupported", "Output image extension must be PNG, JPEG, HEIF, or TIFF");
    }
#ifndef __APPLE__
    (void)image;
    (void)path;
    (void)options;
    return writeError("PlatformUnsupported", "ImageCodec currently uses Apple ImageIO");
#else
    if (image.empty() || image.channels != 4 || image.pixels.size() != image.sampleCount()) {
        return writeError("ImageBufferInvalid", "Image buffer must be non-empty Float32 RGBA");
    }

    auto colorSpace = createOutputColorSpace(options);
    if (colorSpace.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create output color space");
    }

    const bool write16 = shouldWrite16Bit(path, options);
    const bool flattenToBlack = isJpegPath(path);
    const auto rgba = write16 ? createRgba16BigEndian(image, options.colorSpace)
                              : createRgba8(image, options.colorSpace, flattenToBlack);

    CFRef<CGDataProviderRef> provider(CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr));
    if (provider.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create image data provider");
    }

    const auto bitsPerComponent = write16 ? 16 : 8;
    const auto bitsPerPixel = write16 ? 64 : 32;
    const auto bytesPerRow = static_cast<std::size_t>(image.width) * (write16 ? 8 : 4);
    const auto byteOrder = write16 ? kCGBitmapByteOrder16Big : kCGBitmapByteOrder32Big;

    const auto alphaInfo = flattenToBlack ? kCGImageAlphaNoneSkipLast : kCGImageAlphaPremultipliedLast;
    CFRef<CGImageRef> cgImage(CGImageCreate(image.width, image.height, bitsPerComponent, bitsPerPixel, bytesPerRow,
                                            colorSpace.get(), static_cast<CGBitmapInfo>(alphaInfo) | byteOrder,
                                            provider.get(), nullptr, false, kCGRenderingIntentDefault));
    if (cgImage.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create output image");
    }

    auto url = createFileUrl(path);
    if (url.get() == nullptr) {
        return writeError("OutputPathInvalid", "Could not create file URL for output");
    }

    CFRef<CGImageDestinationRef> destination(
        CGImageDestinationCreateWithURL(url.get(), outputTypeForPath(path), 1, nullptr));
    if (destination.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create image destination");
    }

    auto properties = createDestinationProperties(path, options);
    CGImageDestinationAddImage(destination.get(), cgImage.get(), properties.get());
    if (!CGImageDestinationFinalize(destination.get())) {
        return writeError("ImageEncodeFailed", "ImageIO failed to write output image");
    }

    ImageWriteResult result;
    result.ok = true;
    return result;
#endif
}

ImageWriteResult ImageCodec::writeRows(const ImageBuffer& prototype, const std::filesystem::path& path,
                                       const ImageRowProducer& producer) const {
    ImageWriteOptions options;
    if (prototype.colorEncoding == ColorEncoding::Linear && prototype.sourceBitsPerChannel <= 16) {
        options.colorSpace = ImageWriteColorSpace::LinearSRGB;
    }
    return writeRows(prototype, path, producer, options);
}

ImageWriteResult ImageCodec::writeRows(const ImageBuffer& prototype, const std::filesystem::path& path,
                                       const ImageRowProducer& producer, const ImageWriteOptions& options) const {
    if (!validImageWriteOptions(path, options)) {
        return writeError("ArgumentInvalid", "Image write options are invalid");
    }
    if (isFitsPath(path)) {
        return writeError("ImageRowWriteUnsupported", "FITS output requires a complete image buffer");
    }
    if (!isRasterOutputPath(path)) {
        return writeError("OutputFormatUnsupported", "Output image extension must be PNG, JPEG, HEIF, or TIFF");
    }
#ifndef __APPLE__
    (void)prototype;
    (void)path;
    (void)producer;
    (void)options;
    return writeError("PlatformUnsupported", "ImageCodec currently uses Apple ImageIO");
#else
    if (prototype.empty() || prototype.channels != 4 || prototype.pixels.size() != prototype.sampleCount()) {
        return writeError("ImageBufferInvalid", "Image prototype must be non-empty Float32 RGBA");
    }
    if (!producer) {
        return writeError("ArgumentInvalid", "Image row producer must not be empty");
    }

    auto colorSpace = createOutputColorSpace(options);
    if (colorSpace.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create output color space");
    }

    const bool write16 = shouldWrite16Bit(path, options);
    const bool flattenToBlack = isJpegPath(path);
    const std::size_t bytesPerPixel = write16 ? 8 : 4;
    const auto width = static_cast<std::uintmax_t>(prototype.width);
    const auto height = static_cast<std::uintmax_t>(prototype.height);
    if (width > std::numeric_limits<std::uintmax_t>::max() / bytesPerPixel ||
        height > std::numeric_limits<std::uintmax_t>::max() / (width * bytesPerPixel)) {
        return writeError("ImageDimensionsInvalid", "Encoded image size exceeds the supported limit");
    }
    const auto encodedBytes = width * height * bytesPerPixel;

    TemporaryImageRowCache cache;
    const auto cacheResult = createTemporaryImageRowCache(cache, encodedBytes);
    if (!cacheResult.ok) {
        return cacheResult;
    }
    std::ofstream output(cache.pixelPath(), std::ios::binary | std::ios::trunc);
    if (!output) {
        return writeError("TemporaryStorageWriteFailed", "Unable to open the image row cache");
    }

    const auto rowSamples = static_cast<std::size_t>(prototype.width) * prototype.channels;
    std::vector<std::uint8_t> encodedRow(static_cast<std::size_t>(prototype.width) * bytesPerPixel, 0);
    std::uint32_t expectedRow = 0;
    const ImageRowConsumer consumer = [&](std::uint32_t row, const float* samples, std::size_t sampleCount) {
        if (row != expectedRow || row >= prototype.height || samples == nullptr || sampleCount != rowSamples) {
            return false;
        }
        for (std::uint32_t x = 0; x < prototype.width; ++x) {
            const auto sampleOffset = static_cast<std::size_t>(x) * prototype.channels;
            const float alpha = finiteUnitValue(samples[sampleOffset + 3]);
            if (write16) {
                const auto store = [&](std::size_t channel, float value) {
                    const auto encoded = floatToUInt16(value);
                    const auto byteOffset = (static_cast<std::size_t>(x) * prototype.channels + channel) * 2;
                    encodedRow[byteOffset] = static_cast<std::uint8_t>((encoded >> 8U) & 0xFFU);
                    encodedRow[byteOffset + 1] = static_cast<std::uint8_t>(encoded & 0xFFU);
                };
                store(0, colorComponentForOutput(
                    samples[sampleOffset], prototype.colorEncoding, options.colorSpace
                ) * alpha);
                store(1, colorComponentForOutput(
                    samples[sampleOffset + 1], prototype.colorEncoding, options.colorSpace
                ) * alpha);
                store(2, colorComponentForOutput(
                    samples[sampleOffset + 2], prototype.colorEncoding, options.colorSpace
                ) * alpha);
                store(3, alpha);
            } else {
                const auto byteOffset = static_cast<std::size_t>(x) * prototype.channels;
                const auto component = [&](std::size_t channel) {
                    return flattenToBlack
                               ? colorComponentFlattenedToBlack(
                                     samples[sampleOffset + channel], alpha, prototype.colorEncoding, options.colorSpace
                                 )
                               : colorComponentForOutput(
                                     samples[sampleOffset + channel], prototype.colorEncoding, options.colorSpace
                                 ) * alpha;
                };
                encodedRow[byteOffset] = floatToByte(component(0));
                encodedRow[byteOffset + 1] = floatToByte(component(1));
                encodedRow[byteOffset + 2] = floatToByte(component(2));
                encodedRow[byteOffset + 3] = floatToByte(flattenToBlack ? 1.0F : alpha);
            }
        }
        output.write(reinterpret_cast<const char*>(encodedRow.data()),
                     static_cast<std::streamsize>(encodedRow.size()));
        if (!output) {
            return false;
        }
        ++expectedRow;
        return true;
    };

    const bool produced = producer(consumer);
    output.close();
    if (!produced || expectedRow != prototype.height || !output) {
        return writeError(output ? "ImageRowProductionFailed" : "TemporaryStorageWriteFailed",
                          output ? "Image row producer did not provide every row in order"
                                 : "Unable to write the image row cache");
    }

    const auto cachePath = cache.pixelPath().string();
    CFRef<CGDataProviderRef> provider(CGDataProviderCreateWithFilename(cachePath.c_str()));
    if (provider.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create image data provider from the row cache");
    }

    const auto bitsPerComponent = write16 ? 16 : 8;
    const auto bitsPerPixel = write16 ? 64 : 32;
    const auto bytesPerRow = static_cast<std::size_t>(prototype.width) * bytesPerPixel;
    const auto byteOrder = write16 ? kCGBitmapByteOrder16Big : kCGBitmapByteOrder32Big;
    const auto alphaInfo = flattenToBlack ? kCGImageAlphaNoneSkipLast : kCGImageAlphaPremultipliedLast;
    CFRef<CGImageRef> cgImage(CGImageCreate(prototype.width, prototype.height, bitsPerComponent, bitsPerPixel,
                                            bytesPerRow, colorSpace.get(),
                                            static_cast<CGBitmapInfo>(alphaInfo) | byteOrder,
                                            provider.get(), nullptr, false, kCGRenderingIntentDefault));
    if (cgImage.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create output image");
    }

    auto url = createFileUrl(path);
    if (url.get() == nullptr) {
        return writeError("OutputPathInvalid", "Could not create file URL for output");
    }
    CFRef<CGImageDestinationRef> destination(
        CGImageDestinationCreateWithURL(url.get(), outputTypeForPath(path), 1, nullptr));
    if (destination.get() == nullptr) {
        return writeError("ImageEncodeFailed", "Could not create image destination");
    }

    auto properties = createDestinationProperties(path, options);
    CGImageDestinationAddImage(destination.get(), cgImage.get(), properties.get());
    if (!CGImageDestinationFinalize(destination.get())) {
        return writeError("ImageEncodeFailed", "ImageIO failed to write output image");
    }

    ImageWriteResult result;
    result.ok = true;
    return result;
#endif
}

} // namespace photonstack
