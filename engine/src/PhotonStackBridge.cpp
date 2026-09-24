#include "photonstack/PhotonStackBridge.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "photonstack/Curves.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/ImageCodec.hpp"
#include "photonstack/ImageResizer.hpp"
#include "photonstack/MosaicBuilder.hpp"
#include "photonstack/ScientificDisplayBridge.hpp"
#include "photonstack/Version.hpp"

namespace {

photonstack::MosaicProjection bridgeProjection(PhotonStackMosaicProjection projection) {
    return projection == PHOTONSTACK_MOSAIC_PROJECTION_CYLINDRICAL
        ? photonstack::MosaicProjection::Cylindrical
        : photonstack::MosaicProjection::Planar;
}

photonstack::MosaicLayout bridgeLayout(PhotonStackMosaicLayout layout) {
    return layout == PHOTONSTACK_MOSAIC_LAYOUT_GRID ? photonstack::MosaicLayout::Grid : photonstack::MosaicLayout::Horizontal;
}

photonstack::MosaicAlignment bridgeAlignment(PhotonStackMosaicAlignment alignment) {
    return alignment == PHOTONSTACK_MOSAIC_ALIGNMENT_AUTO ? photonstack::MosaicAlignment::Auto : photonstack::MosaicAlignment::Manual;
}

photonstack::MosaicBlendMode bridgeBlendMode(PhotonStackMosaicBlendMode blendMode) {
    switch (blendMode) {
    case PHOTONSTACK_MOSAIC_BLEND_AVERAGE:
        return photonstack::MosaicBlendMode::Average;
    case PHOTONSTACK_MOSAIC_BLEND_MULTIBAND:
        return photonstack::MosaicBlendMode::Multiband;
    case PHOTONSTACK_MOSAIC_BLEND_FEATHER:
    default:
        return photonstack::MosaicBlendMode::Feather;
    }
}

bool validMosaicRequest(const PhotonStackMosaicRequest& request) {
    const bool projectionValid = request.projection == PHOTONSTACK_MOSAIC_PROJECTION_PLANAR ||
                                 request.projection == PHOTONSTACK_MOSAIC_PROJECTION_CYLINDRICAL;
    const bool layoutValid = request.layout == PHOTONSTACK_MOSAIC_LAYOUT_HORIZONTAL ||
                             request.layout == PHOTONSTACK_MOSAIC_LAYOUT_GRID;
    const bool alignmentValid = request.alignment == PHOTONSTACK_MOSAIC_ALIGNMENT_MANUAL ||
                                request.alignment == PHOTONSTACK_MOSAIC_ALIGNMENT_AUTO;
    const bool blendValid = request.blendMode == PHOTONSTACK_MOSAIC_BLEND_AVERAGE ||
                            request.blendMode == PHOTONSTACK_MOSAIC_BLEND_FEATHER ||
                            request.blendMode == PHOTONSTACK_MOSAIC_BLEND_MULTIBAND;
    return projectionValid && layoutValid && alignmentValid && blendValid;
}

bool validRawWhiteBalanceMode(PhotonStackRawWhiteBalanceMode mode) {
    return mode == PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA ||
           mode == PHOTONSTACK_RAW_WHITE_BALANCE_AUTO ||
           mode == PHOTONSTACK_RAW_WHITE_BALANCE_DAYLIGHT ||
           mode == PHOTONSTACK_RAW_WHITE_BALANCE_MANUAL;
}

bool validRawBlackLevelMode(PhotonStackRawBlackLevelMode mode) {
    return mode == PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA ||
           mode == PHOTONSTACK_RAW_BLACK_LEVEL_AUTO ||
           mode == PHOTONSTACK_RAW_BLACK_LEVEL_MANUAL;
}

bool validRawDemosaicQuality(PhotonStackRawDemosaicQuality quality) {
    return quality == PHOTONSTACK_RAW_DEMOSAIC_FAST ||
           quality == PHOTONSTACK_RAW_DEMOSAIC_BALANCED ||
           quality == PHOTONSTACK_RAW_DEMOSAIC_HIGH;
}

bool validCurveChannel(PhotonStackCurveChannel channel) {
    return channel == PHOTONSTACK_CURVE_CHANNEL_RGB ||
           channel == PHOTONSTACK_CURVE_CHANNEL_RED ||
           channel == PHOTONSTACK_CURVE_CHANNEL_GREEN ||
           channel == PHOTONSTACK_CURVE_CHANNEL_BLUE ||
           channel == PHOTONSTACK_CURVE_CHANNEL_LUMINANCE;
}

bool validCurvePreviewRequest(const PhotonStackCurvePreviewRequest& request) {
    return request.width > 0 &&
           request.retainDecodedSource <= 1 &&
           validRawWhiteBalanceMode(request.rawWhiteBalanceMode) &&
           validRawBlackLevelMode(request.rawBlackLevelMode) &&
           validRawDemosaicQuality(request.rawDemosaicQuality) &&
           validCurveChannel(request.curveChannel) &&
           ((request.curvePoints == nullptr && request.curvePointCount == 0) ||
            (request.curvePoints != nullptr));
}

photonstack::RawWhiteBalanceMode bridgeRawWhiteBalanceMode(PhotonStackRawWhiteBalanceMode mode) {
    switch (mode) {
    case PHOTONSTACK_RAW_WHITE_BALANCE_AUTO:
        return photonstack::RawWhiteBalanceMode::Auto;
    case PHOTONSTACK_RAW_WHITE_BALANCE_DAYLIGHT:
        return photonstack::RawWhiteBalanceMode::Daylight;
    case PHOTONSTACK_RAW_WHITE_BALANCE_MANUAL:
        return photonstack::RawWhiteBalanceMode::Manual;
    case PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA:
    default:
        return photonstack::RawWhiteBalanceMode::Camera;
    }
}

photonstack::RawBlackLevelMode bridgeRawBlackLevelMode(PhotonStackRawBlackLevelMode mode) {
    switch (mode) {
    case PHOTONSTACK_RAW_BLACK_LEVEL_AUTO:
        return photonstack::RawBlackLevelMode::Auto;
    case PHOTONSTACK_RAW_BLACK_LEVEL_MANUAL:
        return photonstack::RawBlackLevelMode::Manual;
    case PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA:
    default:
        return photonstack::RawBlackLevelMode::Camera;
    }
}

photonstack::RawDemosaicQuality bridgeRawDemosaicQuality(PhotonStackRawDemosaicQuality quality) {
    switch (quality) {
    case PHOTONSTACK_RAW_DEMOSAIC_FAST:
        return photonstack::RawDemosaicQuality::Fast;
    case PHOTONSTACK_RAW_DEMOSAIC_BALANCED:
        return photonstack::RawDemosaicQuality::Balanced;
    case PHOTONSTACK_RAW_DEMOSAIC_HIGH:
    default:
        return photonstack::RawDemosaicQuality::High;
    }
}

photonstack::CurveChannel bridgeCurveChannel(PhotonStackCurveChannel channel) {
    switch (channel) {
    case PHOTONSTACK_CURVE_CHANNEL_RED:
        return photonstack::CurveChannel::Red;
    case PHOTONSTACK_CURVE_CHANNEL_GREEN:
        return photonstack::CurveChannel::Green;
    case PHOTONSTACK_CURVE_CHANNEL_BLUE:
        return photonstack::CurveChannel::Blue;
    case PHOTONSTACK_CURVE_CHANNEL_LUMINANCE:
        return photonstack::CurveChannel::Luminance;
    case PHOTONSTACK_CURVE_CHANNEL_RGB:
    default:
        return photonstack::CurveChannel::RGB;
    }
}

PhotonStackImageReadBackend bridgeImageReadBackend(photonstack::ImageReadBackend backend) {
    switch (backend) {
    case photonstack::ImageReadBackend::ImageIO:
        return PHOTONSTACK_IMAGE_READ_BACKEND_IMAGEIO;
    case photonstack::ImageReadBackend::AppleRaw:
        return PHOTONSTACK_IMAGE_READ_BACKEND_APPLE_RAW;
    case photonstack::ImageReadBackend::Fits:
        return PHOTONSTACK_IMAGE_READ_BACKEND_FITS;
    case photonstack::ImageReadBackend::Unknown:
    default:
        return PHOTONSTACK_IMAGE_READ_BACKEND_UNKNOWN;
    }
}

photonstack::ImageReadOptions bridgeCurvePreviewReadOptions(const PhotonStackCurvePreviewRequest& request) {
    photonstack::ImageReadOptions options;
    options.fits.debayer = true;
    options.raw.whiteBalanceMode = bridgeRawWhiteBalanceMode(request.rawWhiteBalanceMode);
    options.raw.manualWhiteBalanceTemperature = request.rawManualWhiteBalanceTemperature;
    options.raw.manualWhiteBalanceTint = request.rawManualWhiteBalanceTint;
    options.raw.exposureBias = request.rawExposureBias;
    options.raw.blackLevelMode = bridgeRawBlackLevelMode(request.rawBlackLevelMode);
    options.raw.manualBlackLevel = request.rawManualBlackLevel;
    options.raw.demosaicQuality = bridgeRawDemosaicQuality(request.rawDemosaicQuality);
    options.raw.linearOutput = request.rawLinearOutput != 0;
    return options;
}

struct CurvePreviewSourceKey {
    std::string inputPath;
    std::uintmax_t fileSize = 0;
    std::int64_t modificationTime = 0;
    PhotonStackRawWhiteBalanceMode whiteBalanceMode = PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA;
    std::uint32_t manualWhiteBalanceTemperature = 0;
    std::uint32_t manualWhiteBalanceTint = 0;
    std::uint32_t exposureBias = 0;
    PhotonStackRawBlackLevelMode blackLevelMode = PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA;
    std::uint32_t manualBlackLevel = 0;
    PhotonStackRawDemosaicQuality demosaicQuality = PHOTONSTACK_RAW_DEMOSAIC_HIGH;
    int linearOutput = 0;
    std::uint32_t maximumDecodePixelSize = 0;
    bool needsScientificSource = false;

    bool operator==(const CurvePreviewSourceKey&) const = default;
};

struct CurvePreviewDecodedSource {
    photonstack::ImageBuffer display;
    std::optional<photonstack::ImageBuffer> scientific;
    photonstack::ImageReadBackend backend = photonstack::ImageReadBackend::Unknown;
    bool usedFallback = false;
    std::string fallbackErrorCode;
    std::string fallbackMessage;
};

struct CurvePreviewResizedSource {
    std::shared_ptr<const CurvePreviewDecodedSource> decodedOwner;
    std::optional<photonstack::ImageBuffer> resizedDisplay;
    std::optional<photonstack::ImageBuffer> resizedScientific;

    const photonstack::ImageBuffer& display() const {
        return resizedDisplay.has_value() ? *resizedDisplay : decodedOwner->display;
    }

    const photonstack::ImageBuffer* scientific() const {
        if (resizedScientific.has_value()) {
            return &*resizedScientific;
        }
        if (decodedOwner && decodedOwner->scientific.has_value()) {
            return &*decodedOwner->scientific;
        }
        return nullptr;
    }
};

struct CurvePreviewResizeCacheEntry {
    CurvePreviewSourceKey key;
    std::uint32_t width = 0;
    std::shared_ptr<const CurvePreviewResizedSource> source;
};

std::mutex curvePreviewCacheMutex;
std::optional<CurvePreviewSourceKey> curvePreviewDecodedKey;
std::shared_ptr<const CurvePreviewDecodedSource> curvePreviewDecodedSource;
std::vector<CurvePreviewResizeCacheEntry> curvePreviewResizeCache;

constexpr std::size_t kMaximumCachedCurvePreviewDecodedBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumCachedCurvePreviewResizeBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumCurvePreviewResizeEntries = 2;

std::size_t imageStorageBytes(const photonstack::ImageBuffer& image) {
    if (image.pixels.size() > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return image.pixels.size() * sizeof(float);
}

std::size_t sourceStorageBytes(
    const photonstack::ImageBuffer& display,
    const std::optional<photonstack::ImageBuffer>& scientific
) {
    const auto displayBytes = imageStorageBytes(display);
    const auto scientificBytes = scientific.has_value() ? imageStorageBytes(*scientific) : 0;
    if (displayBytes > std::numeric_limits<std::size_t>::max() - scientificBytes) {
        return std::numeric_limits<std::size_t>::max();
    }
    return displayBytes + scientificBytes;
}

CurvePreviewSourceKey curvePreviewSourceKey(
    const std::filesystem::path& inputPath,
    const PhotonStackCurvePreviewRequest& request,
    bool needsScientificSource
) {
    std::error_code error;
    auto normalizedPath = std::filesystem::weakly_canonical(inputPath, error);
    if (error) {
        error.clear();
        normalizedPath = std::filesystem::absolute(inputPath, error);
        if (error) {
            normalizedPath = inputPath.lexically_normal();
        }
    }

    error.clear();
    const auto fileSize = std::filesystem::file_size(inputPath, error);
    const auto resolvedFileSize = error ? 0 : fileSize;
    error.clear();
    const auto lastWriteTime = std::filesystem::last_write_time(inputPath, error);
    const auto resolvedModificationTime = error ? 0 : static_cast<std::int64_t>(lastWriteTime.time_since_epoch().count());

    return {
        .inputPath = normalizedPath.string(),
        .fileSize = resolvedFileSize,
        .modificationTime = resolvedModificationTime,
        .whiteBalanceMode = request.rawWhiteBalanceMode,
        .manualWhiteBalanceTemperature = std::bit_cast<std::uint32_t>(request.rawManualWhiteBalanceTemperature),
        .manualWhiteBalanceTint = std::bit_cast<std::uint32_t>(request.rawManualWhiteBalanceTint),
        .exposureBias = std::bit_cast<std::uint32_t>(request.rawExposureBias),
        .blackLevelMode = request.rawBlackLevelMode,
        .manualBlackLevel = std::bit_cast<std::uint32_t>(request.rawManualBlackLevel),
        .demosaicQuality = request.rawDemosaicQuality,
        .linearOutput = request.rawLinearOutput,
        .maximumDecodePixelSize = request.retainDecodedSource != 0 ? request.width : 0,
        .needsScientificSource = needsScientificSource,
    };
}

std::shared_ptr<const CurvePreviewDecodedSource> loadCurvePreviewDecodedSource(
    const std::filesystem::path& inputPath,
    const PhotonStackCurvePreviewRequest& request,
    const CurvePreviewSourceKey& key,
    std::string& errorMessage
) {
    std::scoped_lock lock(curvePreviewCacheMutex);
    if (curvePreviewDecodedKey.has_value() && *curvePreviewDecodedKey == key && curvePreviewDecodedSource) {
        return curvePreviewDecodedSource;
    }

    const photonstack::ImageCodec codec;
    auto readOptions = bridgeCurvePreviewReadOptions(request);
    readOptions.maximumDecodePixelSize = key.maximumDecodePixelSize;
    auto readResult = codec.read(inputPath, readOptions);
    if (!readResult.ok) {
        errorMessage = readResult.message.empty() ? readResult.errorCode : readResult.message;
        return nullptr;
    }

    auto decoded = std::make_shared<CurvePreviewDecodedSource>();
    decoded->display = std::move(readResult.image);
    decoded->backend = readResult.backend;
    decoded->usedFallback = readResult.usedFallback;
    decoded->fallbackErrorCode = std::move(readResult.fallbackErrorCode);
    decoded->fallbackMessage = std::move(readResult.fallbackMessage);

    if (key.needsScientificSource) {
        photonstack::ImageReadOptions scientificOptions;
        scientificOptions.fits.mode = photonstack::FitsDecodeMode::Scientific;
        scientificOptions.fits.debayer = true;
        auto scientificRead = codec.read(inputPath, scientificOptions);
        if (!scientificRead.ok) {
            errorMessage = scientificRead.message.empty() ? scientificRead.errorCode : scientificRead.message;
            return nullptr;
        }
        decoded->scientific = std::move(scientificRead.image);
    }

    curvePreviewResizeCache.clear();
    if (sourceStorageBytes(decoded->display, decoded->scientific) <= kMaximumCachedCurvePreviewDecodedBytes) {
        curvePreviewDecodedKey = key;
        curvePreviewDecodedSource = decoded;
    } else {
        curvePreviewDecodedKey.reset();
        curvePreviewDecodedSource.reset();
    }
    return decoded;
}

std::shared_ptr<const CurvePreviewResizedSource> loadCurvePreviewResizedSource(
    const CurvePreviewSourceKey& key,
    const std::shared_ptr<const CurvePreviewDecodedSource>& decoded,
    std::uint32_t width,
    std::string& errorMessage
) {
    std::scoped_lock lock(curvePreviewCacheMutex);
    const auto cached = std::find_if(
        curvePreviewResizeCache.begin(),
        curvePreviewResizeCache.end(),
        [&](const CurvePreviewResizeCacheEntry& entry) { return entry.key == key && entry.width == width; }
    );
    if (cached != curvePreviewResizeCache.end()) {
        const auto result = cached->source;
        if (std::next(cached) != curvePreviewResizeCache.end()) {
            auto entry = std::move(*cached);
            curvePreviewResizeCache.erase(cached);
            curvePreviewResizeCache.push_back(std::move(entry));
        }
        return result;
    }

    auto resized = std::make_shared<CurvePreviewResizedSource>();
    if (width == decoded->display.width) {
        resized->decodedOwner = decoded;
    } else {
        const photonstack::ImageResizer resizer;
        auto displayResize = resizer.resizeToWidth(decoded->display, width);
        if (!displayResize.ok) {
            errorMessage = displayResize.message.empty() ? displayResize.errorCode : displayResize.message;
            return nullptr;
        }
        resized->resizedDisplay = std::move(displayResize.image);
        if (decoded->scientific.has_value()) {
            auto scientificResize = resizer.resizeToWidth(*decoded->scientific, width);
            if (!scientificResize.ok) {
                errorMessage = scientificResize.message.empty() ? scientificResize.errorCode : scientificResize.message;
                return nullptr;
            }
            resized->resizedScientific = std::move(scientificResize.image);
        }
    }

    const auto resizedBytes = resized->resizedDisplay.has_value()
        ? sourceStorageBytes(*resized->resizedDisplay, resized->resizedScientific)
        : sourceStorageBytes(decoded->display, decoded->scientific);
    if (resizedBytes <= kMaximumCachedCurvePreviewResizeBytes) {
        curvePreviewResizeCache.push_back({.key = key, .width = width, .source = resized});
        if (curvePreviewResizeCache.size() > kMaximumCurvePreviewResizeEntries) {
            curvePreviewResizeCache.erase(curvePreviewResizeCache.begin());
        }
    }
    return resized;
}

void releaseCurvePreviewCache(const CurvePreviewSourceKey& key) {
    std::scoped_lock lock(curvePreviewCacheMutex);
    if (!curvePreviewDecodedKey.has_value() || *curvePreviewDecodedKey != key || !curvePreviewDecodedSource) {
        return;
    }
    curvePreviewDecodedKey.reset();
    curvePreviewDecodedSource.reset();
    curvePreviewResizeCache.erase(
        std::remove_if(
            curvePreviewResizeCache.begin(),
            curvePreviewResizeCache.end(),
            [&](const CurvePreviewResizeCacheEntry& entry) { return entry.key == key; }
        ),
        curvePreviewResizeCache.end()
    );
}

void writeError(char* errorBuffer, size_t errorBufferSize, std::string_view message) {
    if (errorBuffer == nullptr || errorBufferSize == 0) {
        return;
    }
    const auto length = std::min(errorBufferSize - 1, message.size());
    std::memcpy(errorBuffer, message.data(), length);
    errorBuffer[length] = '\0';
}

} // namespace

extern "C" const char* photonstack_engine_version(void) {
    return photonstack::kPhotonStackEngineVersion;
}

extern "C" PhotonStackMosaicRequest photonstack_default_mosaic_request(void) {
    PhotonStackMosaicRequest request;
    request.overlapPixels = 0;
    request.projection = PHOTONSTACK_MOSAIC_PROJECTION_PLANAR;
    request.layout = PHOTONSTACK_MOSAIC_LAYOUT_HORIZONTAL;
    request.alignment = PHOTONSTACK_MOSAIC_ALIGNMENT_MANUAL;
    request.blendMode = PHOTONSTACK_MOSAIC_BLEND_FEATHER;
    request.exposureMatching = 1;
    request.columns = 1;
    request.previewWidth = 0;
    return request;
}

extern "C" PhotonStackCurvePreviewRequest photonstack_default_curve_preview_request(void) {
    PhotonStackCurvePreviewRequest request{};
    request.width = 1600;
    request.rawWhiteBalanceMode = PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA;
    request.rawManualWhiteBalanceTemperature = 6500.0F;
    request.rawManualWhiteBalanceTint = 0.0F;
    request.rawExposureBias = 0.0F;
    request.rawBlackLevelMode = PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA;
    request.rawManualBlackLevel = 0.0F;
    request.rawDemosaicQuality = PHOTONSTACK_RAW_DEMOSAIC_HIGH;
    request.rawLinearOutput = 0;
    request.retainDecodedSource = 0;
    request.curveChannel = PHOTONSTACK_CURVE_CHANNEL_RGB;
    request.curvePoints = nullptr;
    request.curvePointCount = 0;
    return request;
}

extern "C" void photonstack_clear_curve_preview_cache(void) {
    std::scoped_lock lock(curvePreviewCacheMutex);
    curvePreviewDecodedKey.reset();
    curvePreviewDecodedSource.reset();
    curvePreviewResizeCache.clear();
}

extern "C" PhotonStackMosaicRunResult photonstack_mosaic_paths(
    const char* const* inputs,
    size_t inputCount,
    const char* output,
    PhotonStackMosaicRequest request,
    char* errorBuffer,
    size_t errorBufferSize
) {
    PhotonStackMosaicRunResult bridgeResult{};
    writeError(errorBuffer, errorBufferSize, {});
    if (inputs == nullptr || inputCount == 0 || output == nullptr) {
        writeError(errorBuffer, errorBufferSize, "mosaic requires input paths and an output path");
        return bridgeResult;
    }
    if (inputCount > std::numeric_limits<std::uint32_t>::max()) {
        writeError(errorBuffer, errorBufferSize, "mosaic input count exceeds the supported limit");
        return bridgeResult;
    }
    if (output[0] == '\0') {
        writeError(errorBuffer, errorBufferSize, "output path must not be empty");
        return bridgeResult;
    }
    if (!validMosaicRequest(request)) {
        writeError(errorBuffer, errorBufferSize, "mosaic request contains an invalid enum value");
        return bridgeResult;
    }

    try {
        std::vector<std::filesystem::path> inputPaths;
        inputPaths.reserve(inputCount);
        for (size_t index = 0; index < inputCount; ++index) {
            if (inputs[index] == nullptr) {
                writeError(errorBuffer, errorBufferSize, "input path must not be null");
                return bridgeResult;
            }
            if (inputs[index][0] == '\0') {
                writeError(errorBuffer, errorBufferSize, "input path must not be empty");
                return bridgeResult;
            }
            inputPaths.emplace_back(inputs[index]);
        }

        photonstack::MosaicOptions options;
        options.overlapPixels = request.overlapPixels;
        options.projection = bridgeProjection(request.projection);
        options.layout = bridgeLayout(request.layout);
        options.alignment = bridgeAlignment(request.alignment);
        options.blendMode = bridgeBlendMode(request.blendMode);
        options.exposureMatching = request.exposureMatching != 0;
        options.columns = request.columns;
        options.previewWidth = request.previewWidth;
        if (options.alignment == photonstack::MosaicAlignment::Auto) {
            options.registration.matchTolerance = 4.0F;
            options.registration.starDetection.sigmaThreshold = 2.0F;
        }

        const photonstack::MosaicBuilder builder;
        const auto mosaic = builder.stitchHorizontal(inputPaths, options);
        if (!mosaic.ok) {
            writeError(errorBuffer, errorBufferSize, mosaic.message.empty() ? mosaic.errorCode : mosaic.message);
            return bridgeResult;
        }

        const photonstack::ImageCodec codec;
        const auto write = codec.write(mosaic.image, output);
        if (!write.ok) {
            writeError(errorBuffer, errorBufferSize, write.message.empty() ? write.errorCode : write.message);
            return bridgeResult;
        }

        bridgeResult.ok = 1;
        bridgeResult.width = mosaic.image.width;
        bridgeResult.height = mosaic.image.height;
        bridgeResult.autoAligned = mosaic.usedAutoAlignment ? 1 : 0;
        bridgeResult.matches = static_cast<uint32_t>(std::min<std::size_t>(
            mosaic.matchedPairs, std::numeric_limits<std::uint32_t>::max()));
        bridgeResult.fallbackPanels = static_cast<uint32_t>(std::min<std::size_t>(
            mosaic.fallbackPanels, std::numeric_limits<std::uint32_t>::max()));
    } catch (const std::bad_alloc&) {
        writeError(errorBuffer, errorBufferSize, "mosaic bridge could not allocate its working set");
    } catch (const std::length_error&) {
        writeError(errorBuffer, errorBufferSize, "mosaic bridge input count exceeds container limits");
    } catch (const std::exception& error) {
        writeError(errorBuffer, errorBufferSize, error.what());
    } catch (...) {
        writeError(errorBuffer, errorBufferSize, "mosaic bridge failed with an unknown internal error");
    }
    return bridgeResult;
}

extern "C" PhotonStackCurvePreviewRunResult photonstack_curve_preview_path(
    const char* input,
    const char* output,
    PhotonStackCurvePreviewRequest request,
    char* fallbackErrorCodeBuffer,
    size_t fallbackErrorCodeBufferSize,
    char* fallbackErrorMessageBuffer,
    size_t fallbackErrorMessageBufferSize,
    char* errorBuffer,
    size_t errorBufferSize
) {
    PhotonStackCurvePreviewRunResult bridgeResult{};
    bridgeResult.backend = PHOTONSTACK_IMAGE_READ_BACKEND_UNKNOWN;
    writeError(errorBuffer, errorBufferSize, {});
    writeError(fallbackErrorCodeBuffer, fallbackErrorCodeBufferSize, {});
    writeError(fallbackErrorMessageBuffer, fallbackErrorMessageBufferSize, {});
    if (input == nullptr || output == nullptr) {
        writeError(errorBuffer, errorBufferSize, "curve preview requires input and output paths");
        return bridgeResult;
    }
    if (input[0] == '\0' || output[0] == '\0') {
        writeError(errorBuffer, errorBufferSize, "curve preview paths must not be empty");
        return bridgeResult;
    }
    if (!validCurvePreviewRequest(request)) {
        writeError(errorBuffer, errorBufferSize, "curve preview request contains an invalid value");
        return bridgeResult;
    }

    try {
        const std::filesystem::path inputPath(input);
        const std::filesystem::path outputPath(output);
        const bool needsScientificSource = photonstack::isFitsPath(inputPath) && photonstack::isFitsPath(outputPath);
        const auto sourceKey = curvePreviewSourceKey(inputPath, request, needsScientificSource);
        std::string loadError;
        const auto decodedSource = loadCurvePreviewDecodedSource(inputPath, request, sourceKey, loadError);
        if (!decodedSource) {
            writeError(errorBuffer, errorBufferSize, loadError);
            return bridgeResult;
        }
        const auto resizedSource = loadCurvePreviewResizedSource(
            sourceKey,
            decodedSource,
            request.width,
            loadError
        );
        if (!resizedSource) {
            writeError(errorBuffer, errorBufferSize, loadError);
            return bridgeResult;
        }

        const auto& displaySource = resizedSource->display();
        photonstack::ImageBuffer outputImage;
        if (request.curvePointCount > 0) {
            photonstack::CurvesOptions curveOptions;
            curveOptions.points.clear();
            curveOptions.points.reserve(request.curvePointCount);
            for (size_t index = 0; index < request.curvePointCount; ++index) {
                curveOptions.points.push_back({
                    .input = request.curvePoints[index].input,
                    .output = request.curvePoints[index].output,
                });
            }
            curveOptions.channel = bridgeCurveChannel(request.curveChannel);

            const photonstack::Curves curves;
            auto curveResult = curves.apply(displaySource, curveOptions);
            if (!curveResult.ok) {
                writeError(errorBuffer, errorBufferSize, curveResult.message.empty() ? curveResult.errorCode : curveResult.message);
                return bridgeResult;
            }
            outputImage = std::move(curveResult.image);
        } else {
            outputImage = displaySource;
        }

        if (const auto* scientificSource = resizedSource->scientific()) {
            const photonstack::ScientificDisplayBridge bridge;
            const auto bridgeCorrection = bridge.applyCorrections(
                *scientificSource,
                displaySource,
                outputImage
            );
            if (!bridgeCorrection.ok) {
                writeError(
                    errorBuffer,
                    errorBufferSize,
                    bridgeCorrection.message.empty() ? bridgeCorrection.errorCode : bridgeCorrection.message
                );
                return bridgeResult;
            }
            outputImage = bridgeCorrection.image;
        }

        const photonstack::ImageCodec codec;
        const auto writeResult = codec.write(outputImage, outputPath);
        if (!writeResult.ok) {
            writeError(errorBuffer, errorBufferSize, writeResult.message.empty() ? writeResult.errorCode : writeResult.message);
            return bridgeResult;
        }

        bridgeResult.ok = 1;
        bridgeResult.width = outputImage.width;
        bridgeResult.height = outputImage.height;
        bridgeResult.decodedWidth = decodedSource->display.width;
        bridgeResult.decodedHeight = decodedSource->display.height;
        bridgeResult.backend = bridgeImageReadBackend(decodedSource->backend);
        bridgeResult.usedFallback = decodedSource->usedFallback ? 1 : 0;
        writeError(fallbackErrorCodeBuffer, fallbackErrorCodeBufferSize, decodedSource->fallbackErrorCode);
        writeError(fallbackErrorMessageBuffer, fallbackErrorMessageBufferSize, decodedSource->fallbackMessage);
        if (request.retainDecodedSource == 0) {
            releaseCurvePreviewCache(sourceKey);
        }
    } catch (const std::bad_alloc&) {
        writeError(errorBuffer, errorBufferSize, "curve preview bridge could not allocate its working set");
    } catch (const std::length_error&) {
        writeError(errorBuffer, errorBufferSize, "curve preview bridge input exceeds container limits");
    } catch (const std::exception& error) {
        writeError(errorBuffer, errorBufferSize, error.what());
    } catch (...) {
        writeError(errorBuffer, errorBufferSize, "curve preview bridge failed with an unknown internal error");
    }
    return bridgeResult;
}
