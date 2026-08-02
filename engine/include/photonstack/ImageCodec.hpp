#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "photonstack/ImageBuffer.hpp"

namespace photonstack {

enum class ImageReadBackend {
    Unknown,
    ImageIO,
    AppleRaw,
    Fits,
};

struct ImageReadResult {
    bool ok = false;
    ImageBuffer image;
    ImageReadBackend backend = ImageReadBackend::Unknown;
    bool usedFallback = false;
    std::string fallbackErrorCode;
    std::string fallbackMessage;
    std::string errorCode;
    std::string message;
};

struct ImageWriteResult {
    bool ok = false;
    std::string errorCode;
    std::string message;
};

enum class ImageWriteBitDepth {
    Auto,
    Eight,
    Sixteen,
};

enum class ImageWriteColorSpace {
    SRGB,
    LinearSRGB,
};

enum class RawWhiteBalanceMode {
    Camera,
    Auto,
    Daylight,
    Manual,
};

enum class RawBlackLevelMode {
    Camera,
    Auto,
    Manual,
};

enum class RawDemosaicQuality {
    Fast,
    Balanced,
    High,
};

enum class FitsDecodeMode {
    DisplayNormalized,
    Scientific,
};

struct RawDecodeOptions {
    RawWhiteBalanceMode whiteBalanceMode = RawWhiteBalanceMode::Camera;
    float manualWhiteBalanceTemperature = 6500.0F;
    float manualWhiteBalanceTint = 0.0F;
    float exposureBias = 0.0F;
    RawBlackLevelMode blackLevelMode = RawBlackLevelMode::Camera;
    float manualBlackLevel = 0.0F;
    RawDemosaicQuality demosaicQuality = RawDemosaicQuality::High;
    bool linearOutput = true;
};

struct FitsDecodeOptions {
    FitsDecodeMode mode = FitsDecodeMode::DisplayNormalized;
    bool maskNonFinitePixels = false;
};

struct ImageReadOptions {
    RawDecodeOptions raw;
    FitsDecodeOptions fits;
    // Zero decodes at source resolution. A positive value asks ImageIO for an
    // orientation-correct proxy whose longest edge does not exceed this size.
    std::uint32_t maximumDecodePixelSize = 0;
};

struct ImageWriteOptions {
    ImageWriteBitDepth bitDepth = ImageWriteBitDepth::Auto;
    ImageWriteColorSpace colorSpace = ImageWriteColorSpace::SRGB;
    float quality = 0.92F;
};

bool isValidRawDecodeOptions(const RawDecodeOptions& options);

const char* imageReadBackendName(ImageReadBackend backend);

bool isSupportedImageWritePath(const std::filesystem::path& path);

ImageWriteBitDepth effectiveImageWriteBitDepth(
    const std::filesystem::path& path,
    const ImageWriteOptions& options
);

// The row pointer remains valid only for the duration of each callback.
using ImageRowConsumer = std::function<bool(std::uint32_t row, const float* samples, std::size_t sampleCount)>;
using ImageRowProducer = std::function<bool(const ImageRowConsumer& consumer)>;

void applyRawDecodeOptions(ImageBuffer& image, const RawDecodeOptions& options);

class ImageCodec {
  public:
    ImageReadResult read(const std::filesystem::path& path) const;
    ImageReadResult read(const std::filesystem::path& path, const ImageReadOptions& options) const;
    ImageWriteResult write(const ImageBuffer& image, const std::filesystem::path& path) const;
    ImageWriteResult write(const ImageBuffer& image, const std::filesystem::path& path, const ImageWriteOptions& options) const;
    ImageWriteResult writeRows(const ImageBuffer& prototype, const std::filesystem::path& path,
                               const ImageRowProducer& producer) const;
    ImageWriteResult writeRows(const ImageBuffer& prototype, const std::filesystem::path& path,
                               const ImageRowProducer& producer, const ImageWriteOptions& options) const;
};

} // namespace photonstack
