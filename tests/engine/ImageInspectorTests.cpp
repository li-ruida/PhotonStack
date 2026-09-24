#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#endif

#include "photonstack/ArtifactTrailRemover.hpp"
#include "photonstack/BackgroundExtractor.hpp"
#include "photonstack/Calibrator.hpp"
#include "photonstack/CloudRemoval.hpp"
#include "photonstack/ComaReducer.hpp"
#include "photonstack/ColorAdjuster.hpp"
#include "photonstack/Curves.hpp"
#include "photonstack/Deconvolution.hpp"
#include "photonstack/DrizzleStacker.hpp"
#include "photonstack/FitsCodec.hpp"
#include "photonstack/FrameNormalizer.hpp"
#include "photonstack/FrameQualityAnalyzer.hpp"
#include "photonstack/Histogram.hpp"
#include "photonstack/ImageCodec.hpp"
#include "photonstack/ImageInspector.hpp"
#include "photonstack/ImageResizer.hpp"
#include "photonstack/LocalContrast.hpp"
#include "photonstack/MasterFrameBuilder.hpp"
#include "photonstack/MeteorLayerComposer.hpp"
#include "photonstack/MosaicBuilder.hpp"
#include "photonstack/NoiseReducer.hpp"
#include "photonstack/PhotonStackBridge.h"
#include "photonstack/Registration.hpp"
#include "photonstack/ScientificDisplayBridge.hpp"
#include "photonstack/Sharpen.hpp"
#include "photonstack/Stacker.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/StarMask.hpp"
#include "photonstack/StarReducer.hpp"
#include "photonstack/Stretch.hpp"
#include "photonstack/TileProcessor.hpp"
#include "photonstack/Version.hpp"

namespace {

std::vector<std::filesystem::path> stackCacheDirectories() {
    std::vector<std::filesystem::path> directories;
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error) {
        return directories;
    }
    for (std::filesystem::directory_iterator iterator(root, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_directory(error) &&
            iterator->path().filename().string().starts_with("photonstack-stack-cache-")) {
            directories.push_back(iterator->path());
        }
    }
    std::sort(directories.begin(), directories.end());
    return directories;
}

std::vector<std::filesystem::path> imageRowCacheDirectories() {
    std::vector<std::filesystem::path> directories;
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error) {
        return directories;
    }
    for (std::filesystem::directory_iterator iterator(root, error), end; !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_directory(error) &&
            iterator->path().filename().string().starts_with("photonstack-image-row-cache-")) {
            directories.push_back(iterator->path());
        }
    }
    std::sort(directories.begin(), directories.end());
    return directories;
}

std::filesystem::path writeBytes(const std::string& name, const std::vector<unsigned char>& bytes) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<unsigned char> readBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void overwriteFitsCardValue(std::vector<unsigned char>& bytes, const std::string& key, const std::string& value) {
    assert(key.size() <= 8);
    assert(value.size() <= 70);
    std::string paddedKey = key;
    paddedKey.resize(8, ' ');
    for (std::size_t offset = 0; offset + 80 <= bytes.size(); offset += 80) {
        if (std::equal(paddedKey.begin(), paddedKey.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
            std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 10),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + 80), ' ');
            std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 10));
            return;
        }
    }
    assert(false && "FITS card not found");
}

#ifdef __APPLE__
std::filesystem::path copyImageWithOrientation(
    const std::filesystem::path& sourcePath,
    const std::string& outputName,
    CFStringRef outputType,
    std::int32_t orientation
) {
    const auto sourceString = sourcePath.string();
    const auto outputPath = std::filesystem::temp_directory_path() / outputName;
    const auto outputString = outputPath.string();
    auto sourceUrl = CFURLCreateFromFileSystemRepresentation(
        nullptr,
        reinterpret_cast<const UInt8*>(sourceString.data()),
        static_cast<CFIndex>(sourceString.size()),
        false
    );
    auto outputUrl = CFURLCreateFromFileSystemRepresentation(
        nullptr,
        reinterpret_cast<const UInt8*>(outputString.data()),
        static_cast<CFIndex>(outputString.size()),
        false
    );
    assert(sourceUrl != nullptr);
    assert(outputUrl != nullptr);

    auto source = CGImageSourceCreateWithURL(sourceUrl, nullptr);
    auto destination = CGImageDestinationCreateWithURL(outputUrl, outputType, 1, nullptr);
    assert(source != nullptr);
    assert(destination != nullptr);

    auto orientationNumber = CFNumberCreate(nullptr, kCFNumberSInt32Type, &orientation);
    assert(orientationNumber != nullptr);
    const void* tiffKeys[] = {kCGImagePropertyTIFFOrientation};
    const void* tiffValues[] = {orientationNumber};
    auto tiffProperties = CFDictionaryCreate(
        nullptr,
        tiffKeys,
        tiffValues,
        1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    const void* propertyKeys[] = {kCGImagePropertyOrientation, kCGImagePropertyTIFFDictionary};
    const void* propertyValues[] = {orientationNumber, tiffProperties};
    auto properties = CFDictionaryCreate(
        nullptr,
        propertyKeys,
        propertyValues,
        2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );
    assert(tiffProperties != nullptr);
    assert(properties != nullptr);

    CGImageDestinationAddImageFromSource(destination, source, 0, properties);
    assert(CGImageDestinationFinalize(destination));

    CFRelease(properties);
    CFRelease(tiffProperties);
    CFRelease(orientationNumber);
    CFRelease(destination);
    CFRelease(source);
    CFRelease(outputUrl);
    CFRelease(sourceUrl);
    return outputPath;
}
#endif

void setRgbaPixel(photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
    image.pixels[offset] = value;
    image.pixels[offset + 1] = value;
    image.pixels[offset + 2] = value;
    image.pixels[offset + 3] = 1.0F;
}

void makeOpaque(photonstack::ImageBuffer& image) {
    if (image.channels != 4) {
        return;
    }
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
}

void addGaussianStar(photonstack::ImageBuffer& image, float centerX, float centerY, float peak) {
    makeOpaque(image);
    const int minX = std::max(0, static_cast<int>(std::floor(centerX - 3.0F)));
    const int maxX = std::min(static_cast<int>(image.width) - 1, static_cast<int>(std::ceil(centerX + 3.0F)));
    const int minY = std::max(0, static_cast<int>(std::floor(centerY - 3.0F)));
    const int maxY = std::min(static_cast<int>(image.height) - 1, static_cast<int>(std::ceil(centerY + 3.0F)));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float value = peak * std::exp(-(dx * dx + dy * dy) / 2.0F);
            const auto offset = (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels;
            image.pixels[offset] = std::max(image.pixels[offset], value);
            image.pixels[offset + 1] = std::max(image.pixels[offset + 1], value);
            image.pixels[offset + 2] = std::max(image.pixels[offset + 2], value);
            image.pixels[offset + 3] = 1.0F;
        }
    }
}

photonstack::ImageWriteResult writeSRGBFixture(
    const photonstack::ImageCodec& codec,
    photonstack::ImageBuffer image,
    const std::filesystem::path& path
) {
    image.colorEncoding = photonstack::ColorEncoding::SRGB;
    return codec.write(image, path);
}

void testPngInspect() {
    const std::vector<unsigned char> png = {
        0x89, 'P',  'N',  'G',  '\r', '\n', 0x1A, '\n', 0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00,
        0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const auto path = writeBytes("photonstack-test.png", png);

    const photonstack::ImageInspector inspector;
    const auto result = inspector.inspect(path);

    assert(result.ok);
    assert(result.metadata.format == photonstack::ImageFormat::PNG);
    assert(result.metadata.width == 32);
    assert(result.metadata.height == 16);
    assert(result.metadata.channels == 3);
    assert(result.metadata.bitsPerChannel == 16);
}

void testBridgeDefaults() {
    assert(std::string(photonstack_engine_version()) == photonstack::kPhotonStackEngineVersion);
    const auto request = photonstack_default_mosaic_request();
    assert(request.overlapPixels == 0);
    assert(request.projection == PHOTONSTACK_MOSAIC_PROJECTION_PLANAR);
    assert(request.layout == PHOTONSTACK_MOSAIC_LAYOUT_HORIZONTAL);
    assert(request.alignment == PHOTONSTACK_MOSAIC_ALIGNMENT_MANUAL);
    assert(request.blendMode == PHOTONSTACK_MOSAIC_BLEND_FEATHER);
    assert(request.exposureMatching == 1);
}

void testTiffInspect() {
    const std::vector<unsigned char> tiff = {
        'I', 'I',  42,   0, 8,    0,    0, 0, 4, 0,    0x00, 0x01, 4, 0,    1,    0, 0, 0, 0x40, 0x01, 0,
        0,   0x01, 0x01, 4, 0,    1,    0, 0, 0, 0xF0, 0x00, 0,    0, 0x02, 0x01, 3, 0, 1, 0,    0,    0,
        16,  0,    0,    0, 0x15, 0x01, 3, 0, 1, 0,    0,    0,    3, 0,    0,    0, 0, 0, 0,    0,
    };
    const auto path = writeBytes("photonstack-test.tiff", tiff);

    const photonstack::ImageInspector inspector;
    const auto result = inspector.inspect(path);

    assert(result.ok);
    assert(result.metadata.format == photonstack::ImageFormat::TIFF);
    assert(result.metadata.width == 320);
    assert(result.metadata.height == 240);
    assert(result.metadata.channels == 3);
    assert(result.metadata.bitsPerChannel == 16);
}

void testRasterInspectUsesBoundedMetadataReads() {
    const std::vector<unsigned char> png = {
        0x89, 'P',  'N',  'G',  '\r', '\n', 0x1A, '\n', 0x00, 0x00, 0x00, 0x0D, 'I',  'H',  'D',  'R',  0x00,
        0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x20, 0x10, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const auto largeTailPath = writeBytes("photonstack-inspect-large-tail.png", png);
    std::error_code error;
    constexpr std::uintmax_t largeTailSize = 64ULL * 1024ULL * 1024ULL;
    std::filesystem::resize_file(largeTailPath, largeTailSize, error);
    assert(!error);

    const photonstack::ImageInspector inspector;
    const auto largeTail = inspector.inspect(largeTailPath);
    assert(largeTail.ok);
    assert(largeTail.metadata.format == photonstack::ImageFormat::PNG);
    assert(largeTail.metadata.width == 64);
    assert(largeTail.metadata.height == 32);
    assert(largeTail.metadata.channels == 4);
    assert(largeTail.metadata.bitsPerChannel == 16);

    const std::vector<unsigned char> jpeg = {
        0xFF, 0xD8,
        0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,
        0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x20, 0x03,
        0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
    };
    const auto jpegPath = writeBytes("photonstack-inspect-segments.jpg", jpeg);
    const auto jpegResult = inspector.inspect(jpegPath);
    assert(jpegResult.ok);
    assert(jpegResult.metadata.format == photonstack::ImageFormat::JPEG);
    assert(jpegResult.metadata.width == 32);
    assert(jpegResult.metadata.height == 16);
    assert(jpegResult.metadata.channels == 3);
    assert(jpegResult.metadata.bitsPerChannel == 8);

    const std::vector<unsigned char> invalidJpeg = {
        0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xFF,
    };
    const auto invalidJpegPath = writeBytes("photonstack-inspect-invalid-segment.jpg", invalidJpeg);
    const auto invalidJpegResult = inspector.inspect(invalidJpegPath);
    assert(!invalidJpegResult.ok);
    assert(invalidJpegResult.errorCode == "ImageHeaderInvalid");

    std::filesystem::remove(largeTailPath, error);
    std::filesystem::remove(jpegPath, error);
    std::filesystem::remove(invalidJpegPath, error);
}

void testMissingFile() {
    const photonstack::ImageInspector inspector;
    const auto result = inspector.inspect("/tmp/photonstack-definitely-missing-file.tiff");

    assert(!result.ok);
    assert(result.errorCode == "InputFileNotFound");
}

void testAverageStack() {
    photonstack::ImageBuffer first;
    first.width = 2;
    first.height = 1;
    first.channels = 4;
    first.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F,
    };

    photonstack::ImageBuffer second;
    second.width = 2;
    second.height = 1;
    second.channels = 4;
    second.pixels = {
        0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F,
    };

    const auto firstPath = std::filesystem::temp_directory_path() / "photonstack-stack-a.png";
    const auto secondPath = std::filesystem::temp_directory_path() / "photonstack-stack-b.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, first, firstPath).ok);
    assert(writeSRGBFixture(codec, second, secondPath).ok);

    const photonstack::Stacker stacker;
    std::vector<photonstack::StackProgress> progressEvents;
    photonstack::StackOptions stackOptions = {.method = photonstack::StackMethod::Average};
    stackOptions.progress = [&](const photonstack::StackProgress& progress) {
        progressEvents.push_back(progress);
    };
    const auto result = stacker.stack({firstPath, secondPath}, stackOptions);

    assert(result.ok);
    assert(progressEvents.size() >= 8);
    assert(progressEvents.front().stage == photonstack::StackProgressStage::Reading);
    assert(progressEvents.back().stage == photonstack::StackProgressStage::Finalizing);
    assert(std::fabs(progressEvents.back().progress - 1.0) < 1.0e-9);
    assert(std::is_sorted(progressEvents.begin(), progressEvents.end(), [](const auto& left, const auto& right) {
        return left.progress < right.progress;
    }));
    assert(std::any_of(progressEvents.begin(), progressEvents.end(), [](const auto& progress) {
        return progress.stage == photonstack::StackProgressStage::Accumulating;
    }));
    assert(result.image.width == 2);
    assert(result.image.height == 1);
    assert(result.image.channels == 4);
    assert(std::fabs(result.image.pixels[0] - 0.5F) < 0.01F);
    assert(std::fabs(result.image.pixels[1] - 0.0F) < 0.01F);
    assert(std::fabs(result.image.pixels[2] - 0.5F) < 0.01F);
    assert(std::fabs(result.image.pixels[4] - 0.5F) < 0.01F);
    assert(std::fabs(result.image.pixels[5] - 0.5F) < 0.01F);
    assert(std::fabs(result.image.pixels[6] - 0.0F) < 0.01F);
}

void testStackIgnoresTransparentFootprints() {
    photonstack::ImageBuffer valid;
    valid.width = 1;
    valid.height = 1;
    valid.channels = 4;
    valid.pixels = {1.0F, 0.0F, 0.0F, 1.0F};

    photonstack::ImageBuffer outsideWarpFootprint;
    outsideWarpFootprint.width = 1;
    outsideWarpFootprint.height = 1;
    outsideWarpFootprint.channels = 4;
    outsideWarpFootprint.pixels = {0.0F, 1.0F, 0.0F, 0.0F};

    const auto validPath = std::filesystem::temp_directory_path() / "photonstack-footprint-valid.png";
    const auto outsidePath = std::filesystem::temp_directory_path() / "photonstack-footprint-outside.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, valid, validPath).ok);
    assert(writeSRGBFixture(codec, outsideWarpFootprint, outsidePath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.average({validPath, outsidePath});

    assert(result.ok);
    assert(result.image.channels == 4);
    assert(std::fabs(result.image.pixels[0] - 1.0F) < 0.01F);
    assert(std::fabs(result.image.pixels[1] - 0.0F) < 0.01F);
    assert(std::fabs(result.image.pixels[2] - 0.0F) < 0.01F);
    assert(std::fabs(result.image.pixels[3] - 0.5F) < 0.01F);
}

void testMedianStack() {
    photonstack::ImageBuffer dark;
    dark.width = 1;
    dark.height = 1;
    dark.channels = 4;
    dark.pixels = {0.0F, 0.0F, 0.0F, 1.0F};

    photonstack::ImageBuffer mid;
    mid.width = 1;
    mid.height = 1;
    mid.channels = 4;
    mid.pixels = {0.5F, 0.5F, 0.5F, 1.0F};

    photonstack::ImageBuffer bright;
    bright.width = 1;
    bright.height = 1;
    bright.channels = 4;
    bright.pixels = {1.0F, 1.0F, 1.0F, 1.0F};

    const auto darkPath = std::filesystem::temp_directory_path() / "photonstack-median-dark.png";
    const auto midPath = std::filesystem::temp_directory_path() / "photonstack-median-mid.png";
    const auto brightPath = std::filesystem::temp_directory_path() / "photonstack-median-bright.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, dark, darkPath).ok);
    assert(writeSRGBFixture(codec, mid, midPath).ok);
    assert(writeSRGBFixture(codec, bright, brightPath).ok);

    const photonstack::Stacker stacker;
    std::vector<photonstack::StackProgress> progressEvents;
    photonstack::StackOptions stackOptions = {.method = photonstack::StackMethod::Median};
    stackOptions.progress = [&](const photonstack::StackProgress& progress) {
        progressEvents.push_back(progress);
    };
    const auto result = stacker.stack({darkPath, midPath, brightPath}, stackOptions);

    assert(result.ok);
    assert(std::any_of(progressEvents.begin(), progressEvents.end(), [](const auto& progress) {
        return progress.stage == photonstack::StackProgressStage::Caching;
    }));
    assert(std::any_of(progressEvents.begin(), progressEvents.end(), [](const auto& progress) {
        return progress.stage == photonstack::StackProgressStage::Combining;
    }));
    assert(std::fabs(progressEvents.back().progress - 1.0) < 1.0e-9);
    assert(std::is_sorted(progressEvents.begin(), progressEvents.end(), [](const auto& left, const auto& right) {
        return left.progress < right.progress;
    }));
    assert(std::fabs(result.image.pixels[0] - 0.5F) < 0.01F);
    assert(std::fabs(result.image.pixels[1] - 0.5F) < 0.01F);
    assert(std::fabs(result.image.pixels[2] - 0.5F) < 0.01F);
}

void testSigmaClipStack() {
    photonstack::ImageBuffer base;
    base.width = 1;
    base.height = 1;
    base.channels = 4;
    base.pixels = {0.2F, 0.2F, 0.2F, 1.0F};

    photonstack::ImageBuffer outlier;
    outlier.width = 1;
    outlier.height = 1;
    outlier.channels = 4;
    outlier.pixels = {1.0F, 1.0F, 1.0F, 1.0F};

    const auto firstPath = std::filesystem::temp_directory_path() / "photonstack-sigma-a.png";
    const auto secondPath = std::filesystem::temp_directory_path() / "photonstack-sigma-b.png";
    const auto outlierPath = std::filesystem::temp_directory_path() / "photonstack-sigma-outlier.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, base, firstPath).ok);
    assert(writeSRGBFixture(codec, base, secondPath).ok);
    assert(writeSRGBFixture(codec, outlier, outlierPath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.sigmaClip({firstPath, secondPath, outlierPath}, {.sigmaLow = 1.0F, .sigmaHigh = 1.0F});

    assert(result.ok);
    assert(std::fabs(result.image.pixels[0] - 0.2F) < 0.02F);
    assert(std::fabs(result.image.pixels[1] - 0.2F) < 0.02F);
    assert(std::fabs(result.image.pixels[2] - 0.2F) < 0.02F);
}

void testPercentileClipStack() {
    photonstack::ImageBuffer low;
    low.width = 1;
    low.height = 1;
    low.channels = 4;
    low.pixels = {0.1F, 0.1F, 0.1F, 1.0F};

    photonstack::ImageBuffer mid;
    mid.width = 1;
    mid.height = 1;
    mid.channels = 4;
    mid.pixels = {0.5F, 0.5F, 0.5F, 1.0F};

    photonstack::ImageBuffer high;
    high.width = 1;
    high.height = 1;
    high.channels = 4;
    high.pixels = {1.0F, 1.0F, 1.0F, 1.0F};

    const auto lowPath = std::filesystem::temp_directory_path() / "photonstack-percentile-low.png";
    const auto midPath = std::filesystem::temp_directory_path() / "photonstack-percentile-mid.png";
    const auto highPath = std::filesystem::temp_directory_path() / "photonstack-percentile-high.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, low, lowPath).ok);
    assert(writeSRGBFixture(codec, mid, midPath).ok);
    assert(writeSRGBFixture(codec, high, highPath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.percentileClip({lowPath, midPath, highPath}, {.low = 0.34F, .high = 0.67F});

    assert(result.ok);
    assert(std::fabs(result.image.pixels[0] - 0.5F) < 0.02F);
}

void testWinsorizedSigmaStack() {
    photonstack::ImageBuffer base;
    base.width = 1;
    base.height = 1;
    base.channels = 4;
    base.pixels = {0.2F, 0.2F, 0.2F, 1.0F};

    photonstack::ImageBuffer outlier;
    outlier.width = 1;
    outlier.height = 1;
    outlier.channels = 4;
    outlier.pixels = {1.0F, 1.0F, 1.0F, 1.0F};

    const auto firstPath = std::filesystem::temp_directory_path() / "photonstack-winsor-a.png";
    const auto secondPath = std::filesystem::temp_directory_path() / "photonstack-winsor-b.png";
    const auto outlierPath = std::filesystem::temp_directory_path() / "photonstack-winsor-outlier.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, base, firstPath).ok);
    assert(writeSRGBFixture(codec, base, secondPath).ok);
    assert(writeSRGBFixture(codec, outlier, outlierPath).ok);

    const photonstack::Stacker stacker;
    const auto result =
        stacker.winsorizedSigmaClip({firstPath, secondPath, outlierPath}, {.sigmaLow = 1.0F, .sigmaHigh = 1.0F});

    assert(result.ok);
    assert(result.image.pixels[0] < 0.5F);
}

void testWeightedStack() {
    photonstack::ImageBuffer first;
    first.width = 5;
    first.height = 5;
    first.channels = 4;
    first.pixels.assign(first.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < first.pixelCount(); ++pixel) {
        first.pixels[pixel * first.channels + 3] = 1.0F;
    }
    const auto center = (static_cast<std::size_t>(2) * first.width + 2) * first.channels;
    first.pixels[center] = 1.0F;
    first.pixels[center + 1] = 1.0F;
    first.pixels[center + 2] = 1.0F;

    photonstack::ImageBuffer second = first;
    second.pixels.assign(second.sampleCount(), 0.4F);
    for (std::size_t pixel = 0; pixel < second.pixelCount(); ++pixel) {
        second.pixels[pixel * second.channels + 3] = 1.0F;
    }

    const auto firstPath = std::filesystem::temp_directory_path() / "photonstack-weighted-a.png";
    const auto secondPath = std::filesystem::temp_directory_path() / "photonstack-weighted-b.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, first, firstPath).ok);
    assert(writeSRGBFixture(codec, second, secondPath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.weightedAverage({firstPath, secondPath});

    assert(result.ok);
    assert(result.image.width == 5);
    assert(result.image.height == 5);
}

void testWeightedStackIgnoresTransparentFootprints() {
    photonstack::ImageBuffer valid;
    valid.width = 1;
    valid.height = 1;
    valid.channels = 4;
    valid.pixels = {0.8F, 0.2F, 0.1F, 1.0F};

    photonstack::ImageBuffer outsideWarpFootprint = valid;
    outsideWarpFootprint.pixels = {0.0F, 1.0F, 0.0F, 0.0F};

    const auto validPath = std::filesystem::temp_directory_path() / "photonstack-weighted-footprint-valid.png";
    const auto outsidePath = std::filesystem::temp_directory_path() / "photonstack-weighted-footprint-outside.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, valid, validPath).ok);
    assert(writeSRGBFixture(codec, outsideWarpFootprint, outsidePath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.weightedAverage({validPath, outsidePath});

    assert(result.ok);
    assert(std::fabs(result.image.pixels[0] - 0.8F) < 0.01F);
    assert(std::fabs(result.image.pixels[1] - 0.2F) < 0.01F);
    assert(std::fabs(result.image.pixels[2] - 0.1F) < 0.01F);
    assert(result.image.pixels[3] > 0.0F);
    assert(result.image.pixels[3] < 1.0F);
}

void testStreamingStackRejectsDimensionMismatch() {
    photonstack::ImageBuffer reference;
    reference.width = 2;
    reference.height = 2;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.5F);

    photonstack::ImageBuffer mismatch = reference;
    mismatch.width = 3;
    mismatch.pixels.assign(mismatch.sampleCount(), 0.5F);

    const auto referencePath = std::filesystem::temp_directory_path() / "photonstack-stream-reference.png";
    const auto mismatchPath = std::filesystem::temp_directory_path() / "photonstack-stream-mismatch.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, reference, referencePath).ok);
    assert(writeSRGBFixture(codec, mismatch, mismatchPath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.average({referencePath, mismatchPath});

    assert(!result.ok);
    assert(result.errorCode == "ImageDimensionsMismatch");
}

void testStreamingStackReportsLateReadFailure() {
    photonstack::ImageBuffer reference;
    reference.width = 1;
    reference.height = 1;
    reference.channels = 4;
    reference.pixels = {0.5F, 0.5F, 0.5F, 1.0F};

    const auto referencePath = std::filesystem::temp_directory_path() / "photonstack-stream-readable.png";
    const auto missingPath = std::filesystem::temp_directory_path() / "photonstack-stream-missing.png";
    std::filesystem::remove(missingPath);
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, reference, referencePath).ok);

    const photonstack::Stacker stacker;
    const auto result = stacker.average({referencePath, missingPath});

    assert(!result.ok);
    assert(result.errorCode == "InputFileNotFound");

    const auto cachesBefore = stackCacheDirectories();
    const auto weightedResult = stacker.stack(
        {referencePath, missingPath},
        {.method = photonstack::StackMethod::WeightedAverage, .alignTranslation = true});
    assert(!weightedResult.ok);
    assert(weightedResult.errorCode == "InputFileNotFound");
    assert(stackCacheDirectories() == cachesBefore);
}

void testRobustStacksIgnoreTransparentFootprintsAndCleanCache() {
    photonstack::ImageBuffer red;
    red.width = 1;
    red.height = 1;
    red.channels = 4;
    red.pixels = {1.0F, 0.0F, 0.0F, 1.0F};

    photonstack::ImageBuffer blue = red;
    blue.pixels = {0.0F, 0.0F, 1.0F, 1.0F};

    photonstack::ImageBuffer transparent = red;
    transparent.pixels = {0.0F, 1.0F, 0.0F, 0.0F};

    const auto redPath = std::filesystem::temp_directory_path() / "photonstack-robust-red.png";
    const auto bluePath = std::filesystem::temp_directory_path() / "photonstack-robust-blue.png";
    const auto transparentPath = std::filesystem::temp_directory_path() / "photonstack-robust-transparent.png";
    const auto missingPath = std::filesystem::temp_directory_path() / "photonstack-robust-missing.png";
    std::filesystem::remove(missingPath);
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, red, redPath).ok);
    assert(writeSRGBFixture(codec, blue, bluePath).ok);
    assert(writeSRGBFixture(codec, transparent, transparentPath).ok);

    const auto cachesBefore = stackCacheDirectories();
    const std::vector<std::filesystem::path> inputs = {redPath, bluePath, transparentPath};
    const photonstack::Stacker stacker;
    const std::vector<photonstack::StackOptions> options = {
        {.method = photonstack::StackMethod::Median},
        {.method = photonstack::StackMethod::SigmaClip, .sigma = {.sigmaLow = 2.0F, .sigmaHigh = 2.0F}},
        {.method = photonstack::StackMethod::WinsorizedSigmaClip,
         .winsorizedSigma = {.sigmaLow = 2.0F, .sigmaHigh = 2.0F}},
        {.method = photonstack::StackMethod::PercentileClip, .percentile = {.low = 0.0F, .high = 1.0F}},
    };

    for (const auto& stackOptions : options) {
        const auto result = stacker.stack(inputs, stackOptions);
        assert(result.ok);
        assert(std::fabs(result.image.pixels[0] - 0.5F) < 0.01F);
        assert(std::fabs(result.image.pixels[1]) < 0.01F);
        assert(std::fabs(result.image.pixels[2] - 0.5F) < 0.01F);
        assert(std::fabs(result.image.pixels[3] - 2.0F / 3.0F) < 0.01F);
    }

    const auto failed = stacker.median({redPath, missingPath});
    assert(!failed.ok);
    assert(failed.errorCode == "InputFileNotFound");
    assert(stackCacheDirectories() == cachesBefore);
}

void testStacksWeightPartialCoverage() {
    photonstack::ImageBuffer low;
    low.width = 1;
    low.height = 1;
    low.channels = 4;
    low.colorEncoding = photonstack::ColorEncoding::Linear;
    low.sourceBitsPerChannel = 32;
    low.pixels = {0.0F, 0.0F, 0.0F, 1.0F};

    photonstack::ImageBuffer high = low;
    high.pixels = {1.0F, 1.0F, 1.0F, 1.0F};
    photonstack::ImageBuffer edge = low;
    edge.pixels = {100.0F, 100.0F, 100.0F, 0.0001F};

    const auto lowPath = std::filesystem::temp_directory_path() / "photonstack-partial-stack-low.fits";
    const auto highPath = std::filesystem::temp_directory_path() / "photonstack-partial-stack-high.fits";
    const auto edgePath = std::filesystem::temp_directory_path() / "photonstack-partial-stack-edge.fits";
    const photonstack::ImageCodec codec;
    assert(codec.write(low, lowPath).ok);
    assert(codec.write(high, highPath).ok);
    assert(codec.write(edge, edgePath).ok);

    const std::vector<std::filesystem::path> inputs = {lowPath, highPath, edgePath};
    const std::vector<photonstack::StackOptions> options = {
        {.method = photonstack::StackMethod::Average},
        {.method = photonstack::StackMethod::Median},
        {.method = photonstack::StackMethod::SigmaClip, .sigma = {.sigmaLow = 2.0F, .sigmaHigh = 2.0F}},
        {.method = photonstack::StackMethod::WinsorizedSigmaClip,
         .winsorizedSigma = {.sigmaLow = 2.0F, .sigmaHigh = 2.0F}},
        {.method = photonstack::StackMethod::PercentileClip, .percentile = {.low = 0.0F, .high = 1.0F}},
    };
    const photonstack::Stacker stacker;
    const float expectedCoverage = 2.0001F / 3.0F;
    for (const auto& stackOptions : options) {
        const auto result = stacker.stack(inputs, stackOptions);
        assert(result.ok);
        assert(result.image.pixels[0] > 0.49F);
        assert(result.image.pixels[0] < 0.52F);
        assert(std::fabs(result.image.pixels[1] - result.image.pixels[0]) < 1.0e-6F);
        assert(std::fabs(result.image.pixels[2] - result.image.pixels[0]) < 1.0e-6F);
        assert(std::fabs(result.image.pixels[3] - expectedCoverage) < 1.0e-5F);
    }
}

void testSingleFrameStacksNormalizeCoverageWithoutCache() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.8F, 0.2F, 0.1F, 0.3F,
        0.4F, 0.5F, 0.6F, 0.0F,
    };
    const auto path = std::filesystem::temp_directory_path() / "photonstack-single-stack.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, image, path).ok);

    const auto cachesBefore = stackCacheDirectories();
    const photonstack::Stacker stacker;
    const auto average = stacker.average({path});
    const auto median = stacker.median({path});
    for (const auto* result : {&average, &median}) {
        assert(result->ok);
        assert(std::fabs(result->image.pixels[0] - 0.8F) < 0.01F);
        assert(std::fabs(result->image.pixels[1] - 0.2F) < 0.01F);
        assert(std::fabs(result->image.pixels[2] - 0.1F) < 0.01F);
        assert(std::fabs(result->image.pixels[3] - 0.3F) < 0.01F);
        assert(std::fabs(result->image.pixels[4]) < 0.01F);
        assert(std::fabs(result->image.pixels[5]) < 0.01F);
        assert(std::fabs(result->image.pixels[6]) < 0.01F);
        assert(std::fabs(result->image.pixels[7]) < 0.01F);
    }
    assert(stackCacheDirectories() == cachesBefore);
}

void testResizeToWidth() {
    photonstack::ImageBuffer input;
    input.width = 4;
    input.height = 2;
    input.channels = 4;
    input.pixels.assign(input.sampleCount(), 1.0F);

    const photonstack::ImageResizer resizer;
    const auto result = resizer.resizeToWidth(input, 2);

    assert(result.ok);
    assert(result.image.width == 2);
    assert(result.image.height == 1);
    assert(result.image.channels == 4);
    assert(result.image.pixels.size() == 8);
}

void testResizeIgnoresColorBehindTransparencyAndPreservesMetadata() {
    photonstack::ImageBuffer input;
    input.width = 2;
    input.height = 1;
    input.channels = 4;
    input.colorEncoding = photonstack::ColorEncoding::SRGB;
    input.sourceBitsPerChannel = 14;
    input.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
    };

    const auto result = photonstack::ImageResizer().resizeToWidth(input, 1);

    assert(result.ok);
    assert(result.image.width == 1);
    assert(result.image.height == 1);
    assert(result.image.colorEncoding == photonstack::ColorEncoding::SRGB);
    assert(result.image.sourceBitsPerChannel == 14);
    assert(std::fabs(result.image.pixels[0] - 1.0F) < 1.0e-6F);
    assert(std::fabs(result.image.pixels[1]) < 1.0e-6F);
    assert(std::fabs(result.image.pixels[2]) < 1.0e-6F);
    assert(std::fabs(result.image.pixels[3] - 0.5F) < 1.0e-6F);

    photonstack::ImageBuffer maximum = input;
    maximum.colorEncoding = photonstack::ColorEncoding::Linear;
    maximum.pixels = {
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(), 1.0F,
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(), 1.0F,
    };
    const auto maximumResult = photonstack::ImageResizer().resizeToWidth(maximum, 1);
    assert(maximumResult.ok);
    for (std::uint16_t channel = 0; channel < 3; ++channel) {
        assert(std::isfinite(maximumResult.image.pixels[channel]));
        assert(maximumResult.image.pixels[channel] == std::numeric_limits<float>::max());
    }

    auto nonFinite = input;
    nonFinite.pixels.front() = std::numeric_limits<float>::infinity();
    const auto nonFiniteResult = photonstack::ImageResizer().resizeToWidth(nonFinite, 1);
    assert(!nonFiniteResult.ok);
    assert(nonFiniteResult.errorCode == "ImageBufferInvalid");
}

void testStarDetection() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 5;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setPixel = [&](std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(2, 2, 1.0F);
    setPixel(1, 1, 0.1F);
    setPixel(3, 3, 0.1F);
    const auto maskedOffset = (static_cast<std::size_t>(3) * image.width + 1) * image.channels;
    image.pixels[maskedOffset] = 100.0F;
    image.pixels[maskedOffset + 1] = 100.0F;
    image.pixels[maskedOffset + 2] = 100.0F;
    image.pixels[maskedOffset + 3] = 0.0F;

    const photonstack::StarDetector detector;
    const auto result = detector.detect(image, {.sigmaThreshold = 2.0F, .minPeak = 0.2F});

    assert(result.ok);
    assert(result.stars.size() == 1);
    assert(std::fabs(result.stars[0].x - 2.0F) < 0.1F);
    assert(std::fabs(result.stars[0].y - 2.0F) < 0.1F);
    assert(result.stars[0].fwhm >= 0.0F);
    assert(result.stars[0].eccentricity >= 0.0F);
    assert(result.stars[0].eccentricity <= 1.0F);

    auto allMasked = image;
    for (std::size_t pixel = 0; pixel < allMasked.pixelCount(); ++pixel) {
        allMasked.pixels[pixel * allMasked.channels + 3] = 0.0F;
    }
    const auto empty = detector.detect(allMasked, {.sigmaThreshold = 2.0F, .minPeak = 0.2F});
    assert(!empty.ok);
    assert(empty.errorCode == "ImageCoverageEmpty");

    photonstack::ImageBuffer partialCoverage;
    partialCoverage.width = 7;
    partialCoverage.height = 7;
    partialCoverage.channels = 4;
    partialCoverage.pixels.assign(partialCoverage.sampleCount(), 0.1F);
    for (std::size_t pixel = 0; pixel < partialCoverage.pixelCount(); ++pixel) {
        partialCoverage.pixels[pixel * partialCoverage.channels + 3] = 1.0F;
    }
    const auto setCoveredPeak = [&](std::uint32_t x, float alpha) {
        const auto offset = (static_cast<std::size_t>(3) * partialCoverage.width + x) * partialCoverage.channels;
        partialCoverage.pixels[offset] = 1.0F;
        partialCoverage.pixels[offset + 1] = 1.0F;
        partialCoverage.pixels[offset + 2] = 1.0F;
        partialCoverage.pixels[offset + 3] = alpha;
    };
    setCoveredPeak(1, 1.0F);
    setCoveredPeak(5, 0.01F);
    const auto covered = detector.detect(partialCoverage, {.sigmaThreshold = 2.0F, .minPeak = 0.2F});
    assert(covered.ok);
    assert(covered.stars.size() == 1);
    assert(std::fabs(covered.stars.front().x - 1.0F) < 0.1F);

    const auto hiddenOffset = (static_cast<std::size_t>(1) * partialCoverage.width + 3) * partialCoverage.channels;
    partialCoverage.pixels[hiddenOffset] = std::numeric_limits<float>::quiet_NaN();
    partialCoverage.pixels[hiddenOffset + 1] = std::numeric_limits<float>::infinity();
    partialCoverage.pixels[hiddenOffset + 2] = -std::numeric_limits<float>::infinity();
    partialCoverage.pixels[hiddenOffset + 3] = 0.0F;
    const auto hiddenNonFinite = detector.detect(partialCoverage, {.sigmaThreshold = 2.0F, .minPeak = 0.2F});
    assert(hiddenNonFinite.ok);
    assert(hiddenNonFinite.stars.size() == 1);

    auto visibleNonFinite = partialCoverage;
    visibleNonFinite.pixels[hiddenOffset + 3] = 1.0F;
    const auto invalidColor = detector.detect(visibleNonFinite, {.sigmaThreshold = 2.0F, .minPeak = 0.2F});
    assert(!invalidColor.ok);
    assert(invalidColor.errorCode == "ImageBufferInvalid");

    auto nonFiniteAlpha = partialCoverage;
    nonFiniteAlpha.pixels[hiddenOffset + 3] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidAlpha = detector.detect(nonFiniteAlpha, {.sigmaThreshold = 2.0F, .minPeak = 0.2F});
    assert(!invalidAlpha.ok);
    assert(invalidAlpha.errorCode == "ImageBufferInvalid");

    photonstack::ImageBuffer scientific;
    scientific.width = 5;
    scientific.height = 5;
    scientific.channels = 1;
    scientific.pixels.assign(scientific.sampleCount(), -std::numeric_limits<float>::max());
    scientific.pixels[2U * scientific.width + 2U] = std::numeric_limits<float>::max();
    const auto extreme = detector.detect(scientific, {.sigmaThreshold = 1.0F, .minPeak = 0.0F});
    assert(extreme.ok);
    assert(extreme.stars.size() == 1);
    assert(std::isfinite(extreme.stars.front().x));
    assert(std::isfinite(extreme.stars.front().y));
    assert(std::isfinite(extreme.stars.front().flux));
    assert(std::isfinite(extreme.stars.front().fwhm));
    assert(std::isfinite(extreme.stars.front().eccentricity));
}

void testFrameQualityAnalyzer() {
    photonstack::ImageBuffer image;
    image.width = 8;
    image.height = 8;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setPixel = [&](std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };
    setPixel(3, 3, 1.0F);
    setPixel(5, 5, 0.8F);

    const photonstack::FrameQualityAnalyzer analyzer;
    const auto qualityOptions = photonstack::FrameQualityOptions{
        .starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}
    };
    const auto unmaskedResult = analyzer.analyze(image, qualityOptions);
    auto maskedBaseline = image;
    maskedBaseline.pixels[3] = 0.0F;
    maskedBaseline.pixels[7] = 0.0F;
    const auto cleanResult = analyzer.analyze(maskedBaseline, qualityOptions);

    auto contaminated = maskedBaseline;
    contaminated.pixels[0] = 100.0F;
    contaminated.pixels[1] = 100.0F;
    contaminated.pixels[2] = 100.0F;
    contaminated.pixels[4] = std::numeric_limits<float>::quiet_NaN();
    const auto result = analyzer.analyze(contaminated, qualityOptions);

    assert(cleanResult.ok);
    assert(result.ok);
    assert(result.starCount == cleanResult.starCount);
    assert(result.medianFwhm == cleanResult.medianFwhm);
    assert(result.medianEccentricity == cleanResult.medianEccentricity);
    assert(result.background == cleanResult.background);
    assert(result.noise == cleanResult.noise);
    assert(result.saturatedFraction == cleanResult.saturatedFraction);
    assert(result.score == cleanResult.score);
    assert(result.starCount >= 1);
    assert(result.background >= 0.0F);
    assert(result.score >= 0.0F);
    assert(result.score <= 1.0F);

    auto visibleNonFinite = contaminated;
    visibleNonFinite.pixels[7] = 1.0F;
    const auto invalidVisible = analyzer.analyze(visibleNonFinite, qualityOptions);
    assert(!invalidVisible.ok);
    assert(invalidVisible.errorCode == "ImageBufferInvalid");

    std::vector<float> luminance(image.pixelCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        luminance[pixel] = image.pixels[offset] * 0.2126F + image.pixels[offset + 1] * 0.7152F +
                           image.pixels[offset + 2] * 0.0722F;
    }
    const auto luminanceResult = analyzer.analyzeLuminance(
        image.width, image.height, luminance,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}});
    assert(luminanceResult.ok);
    assert(unmaskedResult.ok);
    assert(luminanceResult.starCount == unmaskedResult.starCount);
    assert(luminanceResult.medianFwhm == unmaskedResult.medianFwhm);
    assert(luminanceResult.medianEccentricity == unmaskedResult.medianEccentricity);
    assert(luminanceResult.background == unmaskedResult.background);
    assert(luminanceResult.noise == unmaskedResult.noise);
    assert(luminanceResult.saturatedFraction == unmaskedResult.saturatedFraction);
    assert(luminanceResult.score == unmaskedResult.score);

    auto scientific = image;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            scientific.pixels[offset + channel] = -2.0F + 10.0F * image.pixels[offset + channel];
        }
    }
    const auto scientificResult = analyzer.analyze(scientific, qualityOptions);
    assert(scientificResult.ok);
    assert(scientificResult.starCount == unmaskedResult.starCount);
    assert(std::fabs(scientificResult.medianFwhm - unmaskedResult.medianFwhm) < 0.0001F);
    assert(std::fabs(scientificResult.medianEccentricity - unmaskedResult.medianEccentricity) < 0.0001F);
    assert(std::fabs(scientificResult.background - unmaskedResult.background) < 0.0001F);
    assert(std::fabs(scientificResult.noise - unmaskedResult.noise) < 0.0001F);
    assert(std::fabs(scientificResult.saturatedFraction - unmaskedResult.saturatedFraction) < 0.0001F);
    assert(std::fabs(scientificResult.score - unmaskedResult.score) < 0.0001F);

    photonstack::ImageBuffer extremeScientific;
    extremeScientific.width = 8;
    extremeScientific.height = 8;
    extremeScientific.channels = 1;
    extremeScientific.pixels.assign(extremeScientific.sampleCount(), -std::numeric_limits<float>::max());
    extremeScientific.pixels[3U * extremeScientific.width + 3U] = std::numeric_limits<float>::max();
    const auto extremeResult = analyzer.analyze(extremeScientific, qualityOptions);
    assert(extremeResult.ok);
    assert(extremeResult.starCount == 1);
    assert(std::isfinite(extremeResult.background));
    assert(std::isfinite(extremeResult.noise));
    assert(std::isfinite(extremeResult.saturatedFraction));
    assert(std::isfinite(extremeResult.score));

    photonstack::ImageBuffer partialCoverage;
    partialCoverage.width = 8;
    partialCoverage.height = 8;
    partialCoverage.channels = 4;
    partialCoverage.pixels.assign(partialCoverage.sampleCount(), 0.05F);
    makeOpaque(partialCoverage);
    const auto addPeak = [&](std::uint32_t x, float alpha) {
        const auto offset = (static_cast<std::size_t>(4) * partialCoverage.width + x) * partialCoverage.channels;
        partialCoverage.pixels[offset] = 1.0F;
        partialCoverage.pixels[offset + 1] = 1.0F;
        partialCoverage.pixels[offset + 2] = 1.0F;
        partialCoverage.pixels[offset + 3] = alpha;
    };
    addPeak(1, 1.0F);
    addPeak(6, 0.01F);
    const auto partialResult = analyzer.analyze(partialCoverage, qualityOptions);
    assert(partialResult.ok);
    assert(partialResult.starCount == 1);
    assert(partialResult.saturatedFraction < 0.02F);

    auto empty = image;
    for (std::size_t pixel = 0; pixel < empty.pixelCount(); ++pixel) {
        empty.pixels[pixel * empty.channels + 3] = 0.0F;
    }
    const auto emptyResult = analyzer.analyze(empty, qualityOptions);
    assert(!emptyResult.ok);
    assert(emptyResult.errorCode == "ImageCoverageEmpty");

    photonstack::ImageBuffer blank;
    blank.width = 8;
    blank.height = 8;
    blank.channels = 4;
    blank.pixels.assign(blank.sampleCount(), 0.1F);
    makeOpaque(blank);
    const auto blankResult = analyzer.analyze(blank, qualityOptions);
    assert(blankResult.ok);
    assert(blankResult.starCount == 0);
    assert(blankResult.score <= 0.31F);

    const auto invalidLuminance = analyzer.analyzeLuminance(image.width, image.height, std::span<const float>{});
    assert(!invalidLuminance.ok);
    assert(invalidLuminance.errorCode == "ImageBufferInvalid");
}

void testFrameNormalizer() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.2F, 0.2F, 0.2F, 1.0F, 0.4F, 0.4F, 0.4F, 1.0F,
    };

    photonstack::ImageBuffer reference;
    reference.width = 2;
    reference.height = 1;
    reference.channels = 4;
    reference.pixels = {
        0.4F, 0.4F, 0.4F, 1.0F, 0.6F, 0.6F, 0.6F, 1.0F,
    };

    const photonstack::FrameNormalizer normalizer;
    const auto result = normalizer.matchReference(image, reference);

    assert(result.ok);
    assert(result.outputBackground > result.inputBackground);

    const auto local = normalizer.matchReferenceLocal(image, reference, {.columns = 2, .rows = 1});
    assert(local.ok);
    assert(local.image.width == image.width);
    assert(local.image.height == image.height);

    photonstack::ImageBuffer masked;
    masked.width = 3;
    masked.height = 1;
    masked.channels = 4;
    masked.pixels = {
        2.0F, 2.0F, 2.0F, 1.0F,
        4.0F, 4.0F, 4.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    const auto scientific = normalizer.normalize(
        masked,
        {.targetBackground = 0.5F, .clampOutput = false}
    );
    assert(scientific.ok);
    assert(std::fabs(scientific.inputBackground - 3.0F) < 0.001F);
    assert(std::fabs(scientific.image.pixels[0] + 0.5F) < 0.001F);
    assert(std::fabs(scientific.image.pixels[4] - 1.5F) < 0.001F);
    for (std::size_t sample = 8; sample < 12; ++sample) {
        assert(std::fabs(scientific.image.pixels[sample]) < 0.001F);
    }

    const auto display = normalizer.normalize(masked, {.targetBackground = 0.5F});
    assert(display.ok);
    assert(std::fabs(display.image.pixels[0]) < 0.001F);
    assert(std::fabs(display.image.pixels[4] - 1.0F) < 0.001F);

    photonstack::ImageBuffer emptyCoverage = masked;
    emptyCoverage.pixels = {
        0.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    const auto empty = normalizer.normalize(emptyCoverage);
    assert(!empty.ok);
    assert(empty.errorCode == "ImageCoverageEmpty");
    const auto emptyReference = normalizer.matchReference(masked, emptyCoverage);
    assert(!emptyReference.ok);
    assert(emptyReference.errorCode == "ImageCoverageEmpty");

    photonstack::ImageBuffer footprintInput;
    footprintInput.width = 4;
    footprintInput.height = 1;
    footprintInput.channels = 4;
    footprintInput.colorEncoding = photonstack::ColorEncoding::Linear;
    footprintInput.pixels = {
        0.0F, 0.0F, 0.0F, 1.0F,
        2.0F, 2.0F, 2.0F, 1.0F,
        4.0F, 4.0F, 4.0F, 1.0F,
        100.0F, 100.0F, 100.0F, 0.0F,
    };
    photonstack::ImageBuffer footprintReference = footprintInput;
    footprintReference.pixels = {
        -100.0F, -100.0F, -100.0F, 0.0F,
        12.0F, 12.0F, 12.0F, 1.0F,
        16.0F, 16.0F, 16.0F, 1.0F,
        100.0F, 100.0F, 100.0F, 1.0F,
    };
    const auto footprintMatched = normalizer.matchReference(
        footprintInput,
        footprintReference,
        {.clampOutput = false}
    );
    assert(footprintMatched.ok);
    assert(std::fabs(footprintMatched.inputBackground - 3.0F) < 0.001F);
    assert(std::fabs(footprintMatched.outputBackground - 14.0F) < 0.001F);
    assert(std::fabs(footprintMatched.scale - 2.0F) < 0.001F);
    assert(std::fabs(footprintMatched.image.pixels[4] - 12.0F) < 0.001F);
    assert(std::fabs(footprintMatched.image.pixels[8] - 16.0F) < 0.001F);

    const auto targetMatched = normalizer.matchReference(
        footprintInput,
        footprintReference,
        {.targetBackground = 0.25F, .clampOutput = false}
    );
    assert(targetMatched.ok);
    assert(std::fabs(targetMatched.outputBackground - 0.25F) < 0.001F);
    assert(std::fabs(targetMatched.scale - footprintMatched.scale) < 0.001F);

    const auto footprintLocal = normalizer.matchReferenceLocal(
        footprintInput,
        footprintReference,
        {.normalization = {.clampOutput = false}, .columns = 2, .rows = 1}
    );
    assert(footprintLocal.ok);
    assert(std::fabs(footprintLocal.scale - 2.0F) < 0.001F);
    assert(std::fabs(footprintLocal.image.pixels[4] - 12.0F) < 0.001F);
    assert(std::fabs(footprintLocal.image.pixels[8] - 16.0F) < 0.001F);

    const auto targetLocal = normalizer.matchReferenceLocal(
        footprintInput,
        footprintReference,
        {.normalization = {.targetBackground = 0.25F, .clampOutput = false}, .columns = 2, .rows = 1}
    );
    assert(targetLocal.ok);
    assert(std::fabs(targetLocal.outputBackground - 0.25F) < 0.001F);
    assert(std::fabs(targetLocal.scale - footprintLocal.scale) < 0.001F);
    assert(std::fabs(targetLocal.image.pixels[4] + 1.75F) < 0.001F);
    assert(std::fabs(targetLocal.image.pixels[8] - 2.25F) < 0.001F);

    auto disjointInput = footprintInput;
    auto disjointReference = footprintReference;
    for (std::size_t pixel = 0; pixel < disjointInput.pixelCount(); ++pixel) {
        disjointInput.pixels[pixel * disjointInput.channels + 3] = pixel == 0 ? 1.0F : 0.0F;
        disjointReference.pixels[pixel * disjointReference.channels + 3] = pixel == 3 ? 1.0F : 0.0F;
    }
    const auto disjoint = normalizer.matchReference(disjointInput, disjointReference);
    assert(!disjoint.ok);
    assert(disjoint.errorCode == "ImageCoverageEmpty");
}

void testCalibration() {
    photonstack::ImageBuffer light;
    light.width = 1;
    light.height = 1;
    light.channels = 4;
    light.colorEncoding = photonstack::ColorEncoding::Linear;
    light.sourceBitsPerChannel = 32;
    light.pixels = {0.8F, 0.8F, 0.8F, 0.7F};

    photonstack::ImageBuffer dark;
    dark.width = 1;
    dark.height = 1;
    dark.channels = 4;
    dark.pixels = {0.1F, 0.1F, 0.1F, 1.0F};

    photonstack::ImageBuffer bias;
    bias.width = 1;
    bias.height = 1;
    bias.channels = 4;
    bias.pixels = {0.05F, 0.05F, 0.05F, 1.0F};

    const photonstack::Calibrator calibrator;
    const auto result = calibrator.calibrate(
        light,
        {
            .dark = &dark,
            .bias = &bias,
            .darkBiasState = photonstack::CalibrationBiasState::Removed,
        }
    );

    assert(result.ok);
    assert(std::fabs(result.image.pixels[0] - 0.65F) < 0.001F);
    assert(std::fabs(result.image.pixels[1] - 0.65F) < 0.001F);
    assert(std::fabs(result.image.pixels[2] - 0.65F) < 0.001F);
    assert(std::fabs(result.image.pixels[3] - 0.7F) < 0.001F);
    assert(result.image.colorEncoding == photonstack::ColorEncoding::Linear);
    assert(result.image.sourceBitsPerChannel == 32);
    assert(result.subtractedBiasFromLight);
    assert(!result.subtractedBiasFromFlat);

    const auto unknownDarkBias = calibrator.calibrate(light, {.dark = &dark, .bias = &bias});
    assert(!unknownDarkBias.ok);
    assert(unknownDarkBias.errorCode == "CalibrationStateUnknown");

    dark.pixels = {0.1F, 0.1F, 0.1F, 1.0F};
    const auto darkIncludesBias = calibrator.calibrate(
        light,
        {
            .dark = &dark,
            .bias = &bias,
            .darkBiasState = photonstack::CalibrationBiasState::Included,
        }
    );
    assert(darkIncludesBias.ok);
    assert(std::fabs(darkIncludesBias.image.pixels[0] - 0.7F) < 0.001F);
    assert(!darkIncludesBias.subtractedBiasFromLight);

    photonstack::ImageBuffer flatLight;
    flatLight.width = 2;
    flatLight.height = 1;
    flatLight.channels = 4;
    flatLight.colorEncoding = photonstack::ColorEncoding::Linear;
    flatLight.pixels = {
        0.6F, 0.6F, 0.6F, 1.0F,
        0.6F, 0.6F, 0.6F, 1.0F,
    };
    auto flatBias = flatLight;
    flatBias.pixels = {
        0.1F, 0.1F, 0.1F, 1.0F,
        0.1F, 0.1F, 0.1F, 1.0F,
    };
    auto biasedFlat = flatLight;
    biasedFlat.pixels = {
        0.3F, 0.3F, 0.3F, 1.0F,
        0.5F, 0.5F, 0.5F, 1.0F,
    };
    const auto unknownFlatBias = calibrator.calibrate(flatLight, {.bias = &flatBias, .flat = &biasedFlat});
    assert(!unknownFlatBias.ok);
    assert(unknownFlatBias.errorCode == "CalibrationStateUnknown");

    const auto flatIncludesBias = calibrator.calibrate(
        flatLight,
        {
            .bias = &flatBias,
            .flat = &biasedFlat,
            .flatBiasState = photonstack::CalibrationBiasState::Included,
        }
    );
    assert(flatIncludesBias.ok);
    assert(std::fabs(flatIncludesBias.image.pixels[0] - 0.75F) < 0.001F);
    assert(std::fabs(flatIncludesBias.image.pixels[4] - 0.375F) < 0.001F);
    assert(flatIncludesBias.subtractedBiasFromLight);
    assert(flatIncludesBias.subtractedBiasFromFlat);

    const auto flatBiasRemoved = calibrator.calibrate(
        flatLight,
        {
            .bias = &flatBias,
            .flat = &biasedFlat,
            .flatBiasState = photonstack::CalibrationBiasState::Removed,
        }
    );
    assert(flatBiasRemoved.ok);
    assert(std::fabs(flatBiasRemoved.image.pixels[0] - (0.5F * 0.4F / 0.3F)) < 0.001F);
    assert(std::fabs(flatBiasRemoved.image.pixels[4] - (0.5F * 0.4F / 0.5F)) < 0.001F);
    assert(flatBiasRemoved.subtractedBiasFromLight);
    assert(!flatBiasRemoved.subtractedBiasFromFlat);

    const auto missingFlatBias = calibrator.calibrate(
        flatLight,
        {.flat = &biasedFlat, .flatBiasState = photonstack::CalibrationBiasState::Included}
    );
    assert(!missingFlatBias.ok);
    assert(missingFlatBias.errorCode == "CalibrationBiasMissing");
    assert(std::string(photonstack::calibrationBiasStateName(photonstack::CalibrationBiasState::Unknown)) == "unknown");
    assert(std::string(photonstack::calibrationBiasStateName(photonstack::CalibrationBiasState::Included)) == "included");
    assert(std::string(photonstack::calibrationBiasStateName(photonstack::CalibrationBiasState::Removed)) == "removed");
    assert(std::string(photonstack::calibrationBiasStateName(static_cast<photonstack::CalibrationBiasState>(99))) == "unknown");

    dark.pixels = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto scientific = calibrator.calibrate(
        light,
        {.dark = &dark, .clampNegativeValues = false}
    );
    assert(scientific.ok);
    assert(std::fabs(scientific.image.pixels[0] + 0.2F) < 0.001F);
    assert(std::fabs(scientific.image.pixels[3] - 0.7F) < 0.001F);

    photonstack::ImageBuffer maskedLight;
    maskedLight.width = 2;
    maskedLight.height = 1;
    maskedLight.channels = 4;
    maskedLight.pixels = {
        4.0F, 4.0F, 4.0F, 1.0F,
        4.0F, 4.0F, 4.0F, 1.0F,
    };
    photonstack::ImageBuffer maskedFlat = maskedLight;
    maskedFlat.pixels = {
        2.0F, 2.0F, 2.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    const auto maskedFlatResult = calibrator.calibrate(maskedLight, {.flat = &maskedFlat});
    assert(maskedFlatResult.ok);
    assert(std::fabs(maskedFlatResult.image.pixels[0] - 4.0F) < 0.001F);
    assert(std::fabs(maskedFlatResult.image.pixels[1] - 4.0F) < 0.001F);
    assert(std::fabs(maskedFlatResult.image.pixels[2] - 4.0F) < 0.001F);
    assert(std::fabs(maskedFlatResult.image.pixels[3] - 1.0F) < 0.001F);
    for (std::size_t sample = 4; sample < 8; ++sample) {
        assert(std::fabs(maskedFlatResult.image.pixels[sample]) < 0.001F);
    }

    photonstack::ImageBuffer invalidFlat = maskedFlat;
    invalidFlat.pixels = {
        0.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    const auto invalidFlatResult = calibrator.calibrate(maskedLight, {.flat = &invalidFlat});
    assert(!invalidFlatResult.ok);
    assert(invalidFlatResult.errorCode == "CalibrationFlatInvalid");

    photonstack::ImageBuffer maskedDark = maskedLight;
    maskedDark.pixels = {
        1.0F, 1.0F, 1.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    const auto maskedDarkResult = calibrator.calibrate(maskedLight, {.dark = &maskedDark});
    assert(maskedDarkResult.ok);
    assert(std::fabs(maskedDarkResult.image.pixels[0] - 3.0F) < 0.001F);
    assert(std::fabs(maskedDarkResult.image.pixels[1] - 3.0F) < 0.001F);
    assert(std::fabs(maskedDarkResult.image.pixels[2] - 3.0F) < 0.001F);
    assert(std::fabs(maskedDarkResult.image.pixels[3] - 1.0F) < 0.001F);
    for (std::size_t sample = 4; sample < 8; ++sample) {
        assert(std::fabs(maskedDarkResult.image.pixels[sample]) < 0.001F);
    }

    photonstack::ImageBuffer partialDark = maskedLight;
    partialDark.pixels = {
        1.0F, 1.0F, 1.0F, 0.25F,
        1.0F, 1.0F, 1.0F, 1.0F,
    };
    maskedLight.pixels[3] = 0.8F;
    const auto partialDarkResult = calibrator.calibrate(maskedLight, {.dark = &partialDark});
    assert(partialDarkResult.ok);
    assert(std::fabs(partialDarkResult.image.pixels[0] - 3.0F) < 0.001F);
    assert(std::fabs(partialDarkResult.image.pixels[3] - 0.25F) < 0.001F);
    assert(std::fabs(partialDarkResult.image.pixels[7] - 1.0F) < 0.001F);

    photonstack::ImageBuffer weightedFlat = maskedLight;
    weightedFlat.pixels = {
        2.0F, 2.0F, 2.0F, 1.0F,
        100.0F, 100.0F, 100.0F, 0.0001F,
    };
    maskedLight.pixels[3] = 1.0F;
    const auto weightedFlatResult = calibrator.calibrate(maskedLight, {.flat = &weightedFlat});
    assert(weightedFlatResult.ok);
    const float expectedFlatMean = (2.0F + 100.0F * 0.0001F) / 1.0001F;
    assert(std::fabs(weightedFlatResult.image.pixels[0] - expectedFlatMean * 2.0F) < 0.001F);
    assert(std::fabs(weightedFlatResult.image.pixels[4] - expectedFlatMean * 0.04F) < 0.001F);
    assert(std::fabs(weightedFlatResult.image.pixels[7] - 0.0001F) < 1.0e-6F);

    photonstack::ImageBuffer smallLight;
    smallLight.width = 2;
    smallLight.height = 1;
    smallLight.channels = 4;
    smallLight.colorEncoding = photonstack::ColorEncoding::Linear;
    smallLight.pixels = {
        2.0e-10F, 2.0e-10F, 2.0e-10F, 1.0F,
        2.0e-10F, 2.0e-10F, 2.0e-10F, 1.0F,
    };
    auto smallFlat = smallLight;
    smallFlat.pixels = {
        1.0e-10F, 1.0e-10F, 1.0e-10F, 1.0F,
        2.0e-10F, 2.0e-10F, 2.0e-10F, 1.0F,
    };
    const auto smallFlatResult = calibrator.calibrate(smallLight, {.flat = &smallFlat});
    assert(smallFlatResult.ok);
    assert(std::fabs(smallFlatResult.image.pixels[0] - 3.0e-10F) < 1.0e-15F);
    assert(std::fabs(smallFlatResult.image.pixels[4] - 1.5e-10F) < 1.0e-15F);

    auto invalidVisible = light;
    invalidVisible.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidVisibleResult = calibrator.calibrate(invalidVisible, {});
    assert(!invalidVisibleResult.ok);
    assert(invalidVisibleResult.errorCode == "ImageBufferInvalid");

    auto hiddenInvalid = maskedLight;
    hiddenInvalid.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    hiddenInvalid.pixels[3] = 0.0F;
    const auto hiddenInvalidResult = calibrator.calibrate(hiddenInvalid, {});
    assert(hiddenInvalidResult.ok);
    assert(hiddenInvalidResult.image.pixels[0] == 0.0F);
    assert(hiddenInvalidResult.image.pixels[3] == 0.0F);

    photonstack::ImageBuffer extremeLight;
    extremeLight.width = 1;
    extremeLight.height = 1;
    extremeLight.channels = 4;
    extremeLight.colorEncoding = photonstack::ColorEncoding::Linear;
    extremeLight.pixels = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        1.0F,
    };
    auto extremeDark = extremeLight;
    extremeDark.pixels[0] = -std::numeric_limits<float>::max();
    extremeDark.pixels[1] = -std::numeric_limits<float>::max();
    extremeDark.pixels[2] = -std::numeric_limits<float>::max();
    const auto overflow = calibrator.calibrate(
        extremeLight,
        {.dark = &extremeDark, .clampNegativeValues = false}
    );
    assert(!overflow.ok);
    assert(overflow.errorCode == "ImageValueInvalid");

    auto srgbLight = light;
    srgbLight.colorEncoding = photonstack::ColorEncoding::SRGB;
    const auto srgbResult = calibrator.calibrate(srgbLight, {});
    assert(!srgbResult.ok);
    assert(srgbResult.errorCode == "ImageColorEncodingMismatch");
}

void testBackgroundExtraction() {
    photonstack::ImageBuffer image;
    image.width = 3;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.2F, 0.2F, 0.2F, 1.0F, 0.2F, 0.2F, 0.2F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
    };

    const photonstack::BackgroundExtractor extractor;
    const auto result = extractor.extractGlobal(image, {.mode = photonstack::BackgroundMode::Subtract});

    assert(result.ok);
    assert(std::fabs(result.background - 0.2F) < 0.001F);
    assert(std::fabs(result.image.pixels[0] - 0.0F) < 0.001F);
    assert(std::fabs(result.image.pixels[8] - 0.8F) < 0.001F);
    assert(std::fabs(result.image.pixels[3] - 1.0F) < 0.001F);

    photonstack::ImageBuffer masked;
    masked.width = 3;
    masked.height = 1;
    masked.channels = 4;
    masked.pixels = {
        2.0F, 2.0F, 2.0F, 1.0F,
        4.0F, 4.0F, 4.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    const auto scientific = extractor.extractGlobal(
        masked,
        {.mode = photonstack::BackgroundMode::Subtract, .clampOutput = false}
    );
    assert(scientific.ok);
    assert(std::fabs(scientific.background - 3.0F) < 0.001F);
    assert(std::fabs(scientific.image.pixels[0] + 1.0F) < 0.001F);
    assert(std::fabs(scientific.image.pixels[4] - 1.0F) < 0.001F);
    for (std::size_t sample = 8; sample < 12; ++sample) {
        assert(std::fabs(scientific.image.pixels[sample]) < 0.001F);
    }

    const auto display = extractor.extractGlobal(masked, {.mode = photonstack::BackgroundMode::Subtract});
    assert(display.ok);
    assert(std::fabs(display.image.pixels[0]) < 0.001F);
    assert(std::fabs(display.image.pixels[4] - 1.0F) < 0.001F);

    photonstack::ImageBuffer emptyCoverage = masked;
    emptyCoverage.pixels.assign(emptyCoverage.sampleCount(), 0.0F);
    const auto emptyGlobal = extractor.extractGlobal(emptyCoverage, {});
    assert(!emptyGlobal.ok);
    assert(emptyGlobal.errorCode == "ImageCoverageEmpty");
    const auto emptyGrid = extractor.extractGrid(emptyCoverage, {.columns = 3, .rows = 1});
    assert(!emptyGrid.ok);
    assert(emptyGrid.errorCode == "ImageCoverageEmpty");

    photonstack::ImageBuffer weightedBackground;
    weightedBackground.width = 3;
    weightedBackground.height = 1;
    weightedBackground.channels = 4;
    weightedBackground.pixels = {
        2.0F, 2.0F, 2.0F, 1.0F,
        4.0F, 4.0F, 4.0F, 1.0F,
        100.0F, 100.0F, 100.0F, 0.0001F,
    };
    const auto weightedGlobal = extractor.extractGlobal(
        weightedBackground,
        {.mode = photonstack::BackgroundMode::Subtract, .strength = 0.0F, .clampOutput = false}
    );
    assert(weightedGlobal.ok);
    assert(std::fabs(weightedGlobal.background - 3.0F) < 0.001F);
    assert(weightedGlobal.image.pixels == weightedBackground.pixels);
    const auto weightedGrid = extractor.extractGrid(
        weightedBackground,
        {.extraction = {.mode = photonstack::BackgroundMode::Subtract,
                        .strength = 0.0F,
                        .clampOutput = false},
         .columns = 1,
         .rows = 1,
         .protectBrightTargets = false}
    );
    assert(weightedGrid.ok);
    assert(std::fabs(weightedGrid.background - 3.0F) < 0.001F);
    assert(std::fabs(weightedGrid.backgroundGrid[0] - 3.0F) < 0.001F);

    photonstack::ImageBuffer targetContamination;
    targetContamination.width = 10;
    targetContamination.height = 1;
    targetContamination.channels = 4;
    targetContamination.pixels.assign(targetContamination.sampleCount(), 0.8F);
    for (std::size_t pixel = 0; pixel < targetContamination.pixelCount(); ++pixel) {
        const auto offset = pixel * targetContamination.channels;
        targetContamination.pixels[offset + 3] = 1.0F;
        if (pixel < 4) {
            targetContamination.pixels[offset] = 0.2F;
            targetContamination.pixels[offset + 1] = 0.2F;
            targetContamination.pixels[offset + 2] = 0.2F;
        }
    }
    const auto protectedTarget = extractor.extractGrid(
        targetContamination,
        {.extraction = {.mode = photonstack::BackgroundMode::Subtract,
                        .strength = 0.0F,
                        .clampOutput = false},
         .columns = 1,
         .rows = 1}
    );
    assert(protectedTarget.ok);
    assert(std::fabs(protectedTarget.backgroundGrid[0] - 0.2F) < 0.001F);
    const auto legacyTarget = extractor.extractGrid(
        targetContamination,
        {.extraction = {.mode = photonstack::BackgroundMode::Subtract,
                        .strength = 0.0F,
                        .clampOutput = false},
         .columns = 1,
         .rows = 1,
         .protectBrightTargets = false}
    );
    assert(legacyTarget.ok);
    assert(std::fabs(legacyTarget.backgroundGrid[0] - 0.8F) < 0.001F);

    photonstack::ImageBuffer irregularFootprint;
    irregularFootprint.width = 4;
    irregularFootprint.height = 1;
    irregularFootprint.channels = 4;
    irregularFootprint.pixels = {
        0.2F, 0.2F, 0.2F, 1.0F,
        0.4F, 0.4F, 0.4F, 1.0F,
        9.0F, 9.0F, 9.0F, 0.0F,
        9.0F, 9.0F, 9.0F, 0.0F,
    };
    const auto protectedFootprint = extractor.extractGrid(
        irregularFootprint,
        {.extraction = {.mode = photonstack::BackgroundMode::Subtract,
                        .strength = 0.0F,
                        .clampOutput = false},
         .columns = 4,
         .rows = 1}
    );
    assert(protectedFootprint.ok);
    assert(protectedFootprint.sampledGridCells == 2);
    assert(protectedFootprint.filledGridCells == 2);
    assert(std::fabs(protectedFootprint.backgroundGrid[0] - 0.2F) < 0.001F);
    assert(std::fabs(protectedFootprint.backgroundGrid[1] - 0.4F) < 0.001F);
    assert(std::fabs(protectedFootprint.backgroundGrid[2] - 0.4F) < 0.001F);
    assert(std::fabs(protectedFootprint.backgroundGrid[3] - 0.4F) < 0.001F);
    const auto legacyFootprint = extractor.extractGrid(
        irregularFootprint,
        {.extraction = {.mode = photonstack::BackgroundMode::Subtract,
                        .strength = 0.0F,
                        .clampOutput = false},
         .columns = 4,
         .rows = 1,
         .protectBrightTargets = false}
    );
    assert(legacyFootprint.ok);
    assert(std::fabs(legacyFootprint.backgroundGrid[2] - 0.3F) < 0.001F);
    assert(std::fabs(legacyFootprint.backgroundGrid[3] - 0.3F) < 0.001F);

    auto invalidVisible = irregularFootprint;
    invalidVisible.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidVisibleGlobal = extractor.extractGlobal(invalidVisible, {});
    assert(!invalidVisibleGlobal.ok);
    assert(invalidVisibleGlobal.errorCode == "ImageBufferInvalid");
    const auto invalidVisibleGrid = extractor.extractGrid(invalidVisible, {.columns = 4, .rows = 1});
    assert(!invalidVisibleGrid.ok);
    assert(invalidVisibleGrid.errorCode == "ImageBufferInvalid");

    auto invalidAlpha = irregularFootprint;
    invalidAlpha.pixels[3] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidAlphaResult = extractor.extractGlobal(invalidAlpha, {});
    assert(!invalidAlphaResult.ok);
    assert(invalidAlphaResult.errorCode == "ImageBufferInvalid");

    auto hiddenInvalid = irregularFootprint;
    hiddenInvalid.pixels[8] = std::numeric_limits<float>::quiet_NaN();
    const auto hiddenInvalidResult = extractor.extractGrid(
        hiddenInvalid,
        {.extraction = {.strength = 0.0F, .clampOutput = false}, .columns = 4, .rows = 1}
    );
    assert(hiddenInvalidResult.ok);
    for (std::size_t sample = 8; sample < 12; ++sample) {
        assert(std::fabs(hiddenInvalidResult.image.pixels[sample]) < 0.001F);
    }

    photonstack::ImageBuffer negativeBackground = weightedBackground;
    negativeBackground.pixels = {
        -4.0F, -4.0F, -4.0F, 1.0F,
        -2.0F, -2.0F, -2.0F, 1.0F,
        -1.0F, -1.0F, -1.0F, 1.0F,
    };
    const auto invalidDivide = extractor.extractGlobal(
        negativeBackground,
        {.mode = photonstack::BackgroundMode::Divide, .clampOutput = false}
    );
    assert(!invalidDivide.ok);
    assert(invalidDivide.errorCode == "BackgroundDivisionInvalid");
    const auto divideNoOp = extractor.extractGrid(
        negativeBackground,
        {.extraction = {.mode = photonstack::BackgroundMode::Divide,
                        .strength = 0.0F,
                        .clampOutput = false},
         .columns = 3,
         .rows = 1}
    );
    assert(divideNoOp.ok);
    assert(divideNoOp.image.pixels == negativeBackground.pixels);

    const auto globalPreserved = extractor.extractGlobal(
        masked,
        {.mode = photonstack::BackgroundMode::Divide, .clampOutput = false}
    );
    assert(globalPreserved.ok);
    assert(globalPreserved.image.pixels == masked.pixels);
    const auto globalUnity = extractor.extractGlobal(
        masked,
        {.mode = photonstack::BackgroundMode::Divide,
         .preserveBrightness = false,
         .clampOutput = false}
    );
    assert(globalUnity.ok);
    assert(std::fabs(globalUnity.image.pixels[0] - 2.0F / 3.0F) < 0.001F);
    assert(std::fabs(globalUnity.image.pixels[4] - 4.0F / 3.0F) < 0.001F);

    photonstack::ImageBuffer gridGradient;
    gridGradient.width = 2;
    gridGradient.height = 1;
    gridGradient.channels = 4;
    gridGradient.pixels = {
        2.0F, 2.0F, 2.0F, 1.0F,
        4.0F, 4.0F, 4.0F, 1.0F,
    };
    const auto preservedGradient = extractor.extractGrid(
        gridGradient,
        {.extraction = {.mode = photonstack::BackgroundMode::Divide, .clampOutput = false},
         .columns = 2,
         .rows = 1}
    );
    assert(preservedGradient.ok);
    assert(std::fabs(preservedGradient.background - 3.0F) < 0.001F);
    assert(std::fabs(preservedGradient.backgroundGrid[0] - 2.0F) < 0.001F);
    assert(std::fabs(preservedGradient.backgroundGrid[1] - 4.0F) < 0.001F);
    assert(std::fabs(preservedGradient.image.pixels[0] - 3.0F) < 0.001F);
    assert(std::fabs(preservedGradient.image.pixels[4] - 3.0F) < 0.001F);

    const auto halfGradient = extractor.extractGrid(
        gridGradient,
        {.extraction = {.mode = photonstack::BackgroundMode::Divide,
                        .strength = 0.5F,
                        .clampOutput = false},
         .columns = 2,
         .rows = 1}
    );
    assert(halfGradient.ok);
    assert(std::fabs(halfGradient.image.pixels[0] - 2.5F) < 0.001F);
    assert(std::fabs(halfGradient.image.pixels[4] - 3.5F) < 0.001F);

    const auto unityGradient = extractor.extractGrid(
        gridGradient,
        {.extraction = {.mode = photonstack::BackgroundMode::Divide,
                        .preserveBrightness = false,
                        .clampOutput = false},
         .columns = 2,
         .rows = 1}
    );
    assert(unityGradient.ok);
    assert(std::fabs(unityGradient.image.pixels[0] - 1.0F) < 0.001F);
    assert(std::fabs(unityGradient.image.pixels[4] - 1.0F) < 0.001F);

    photonstack::ImageBuffer overflowingBackground = weightedBackground;
    const float maximum = std::numeric_limits<float>::max();
    overflowingBackground.pixels = {
        2.0e-6F, 2.0e-6F, 2.0e-6F, 1.0F,
        2.0e-6F, 2.0e-6F, 2.0e-6F, 1.0F,
        maximum, maximum, maximum, 1.0F,
    };
    const auto overflowDivide = extractor.extractGlobal(
        overflowingBackground,
        {.mode = photonstack::BackgroundMode::Divide,
         .preserveBrightness = false,
         .clampOutput = false}
    );
    assert(!overflowDivide.ok);
    assert(overflowDivide.errorCode == "ImageValueInvalid");
}

void testCloudDetectionAndRemoval() {
    photonstack::ImageBuffer image;
    image.width = 80;
    image.height = 48;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.08F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 12; y < 34; ++y) {
        for (std::uint32_t x = 18; x < 58; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.22F;
            image.pixels[offset + 1] = 0.22F;
            image.pixels[offset + 2] = 0.22F;
        }
    }

    const photonstack::CloudRemoval clouds;
    photonstack::CloudRemovalOptions options;
    options.columns = 16;
    options.rows = 12;
    options.minBrightnessDelta = 0.04F;
    options.minCoverage = 0.01F;
    options.strength = 0.7F;
    const auto detected = clouds.detect(image, options);
    assert(detected.ok);
    assert(!detected.regions.empty());

    options.selectedIndices = {0};
    const auto removed = clouds.remove(image, options);
    assert(removed.ok);
    assert(removed.removedRegions == 1);
    const auto cloudCenter = (static_cast<std::size_t>(24) * image.width + 40) * image.channels;
    const auto clearCorner = (static_cast<std::size_t>(4) * image.width + 4) * image.channels;
    assert(removed.image.pixels[cloudCenter] < image.pixels[cloudCenter]);
    assert(removed.image.pixels[clearCorner] == image.pixels[clearCorner]);
}

void testCloudDetectionRespectsCoverageAndRejectsVisibleNonFinitePixels() {
    photonstack::ImageBuffer baseline;
    baseline.width = 80;
    baseline.height = 48;
    baseline.channels = 4;
    baseline.pixels.assign(baseline.sampleCount(), 0.08F);
    for (std::size_t pixel = 0; pixel < baseline.pixelCount(); ++pixel) {
        baseline.pixels[pixel * baseline.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 12; y < 34; ++y) {
        for (std::uint32_t x = 18; x < 58; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * baseline.width + x) * baseline.channels;
            baseline.pixels[offset] = 0.22F;
            baseline.pixels[offset + 1] = 0.22F;
            baseline.pixels[offset + 2] = 0.22F;
        }
    }

    auto hiddenGarbage = baseline;
    for (std::uint32_t y = 0; y < baseline.height; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * baseline.width + x) * baseline.channels;
            baseline.pixels[offset] = 0.0F;
            baseline.pixels[offset + 1] = 0.0F;
            baseline.pixels[offset + 2] = 0.0F;
            baseline.pixels[offset + 3] = 0.0F;
            hiddenGarbage.pixels[offset] = 100.0F;
            hiddenGarbage.pixels[offset + 1] = std::numeric_limits<float>::quiet_NaN();
            hiddenGarbage.pixels[offset + 2] = -100.0F;
            hiddenGarbage.pixels[offset + 3] = 0.0F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 16;
    options.rows = 12;
    options.minBrightnessDelta = 0.04F;
    options.minCoverage = 0.01F;
    const photonstack::CloudRemoval clouds;
    const auto clean = clouds.detect(baseline, options);
    const auto contaminated = clouds.detect(hiddenGarbage, options);
    assert(clean.ok);
    assert(contaminated.ok);
    assert(clean.regions.size() == contaminated.regions.size());
    for (std::size_t index = 0; index < clean.regions.size(); ++index) {
        assert(std::fabs(clean.regions[index].x - contaminated.regions[index].x) < 1.0e-6F);
        assert(std::fabs(clean.regions[index].y - contaminated.regions[index].y) < 1.0e-6F);
        assert(std::fabs(clean.regions[index].confidence - contaminated.regions[index].confidence) < 1.0e-6F);
    }

    auto transparent = baseline;
    for (std::size_t pixel = 0; pixel < transparent.pixelCount(); ++pixel) {
        transparent.pixels[pixel * transparent.channels + 3] = 0.0F;
    }
    const auto emptyCoverage = clouds.detect(transparent, options);
    assert(!emptyCoverage.ok);
    assert(emptyCoverage.errorCode == "ImageCoverageEmpty");

    auto visibleInvalid = baseline;
    visibleInvalid.pixels[16 * visibleInvalid.channels] = std::numeric_limits<float>::infinity();
    const auto invalid = clouds.detect(visibleInvalid, options);
    assert(!invalid.ok);
    assert(invalid.errorCode == "ImageBufferInvalid");
}

void testCloudDetectionPrefersSmoothSparseHazeOverStarField() {
    photonstack::ImageBuffer image;
    image.width = 120;
    image.height = 80;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.045F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    for (std::uint32_t y = 18; y < 56; ++y) {
        for (std::uint32_t x = 36; x < 104; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.12F;
            image.pixels[offset + 1] = 0.12F;
            image.pixels[offset + 2] = 0.13F;
        }
    }
    for (std::uint32_t y = 20; y < 54; y += 3) {
        for (std::uint32_t x = 40; x < 100; x += 5) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.9F;
            image.pixels[offset + 1] = 0.9F;
            image.pixels[offset + 2] = 0.92F;
        }
    }
    for (std::uint32_t y = 50; y < 76; ++y) {
        for (std::uint32_t x = 4; x < 38; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.095F;
            image.pixels[offset + 1] = 0.10F;
            image.pixels[offset + 2] = 0.095F;
        }
    }

    const photonstack::CloudRemoval clouds;
    photonstack::CloudRemovalOptions options;
    options.columns = 24;
    options.rows = 16;
    options.minBrightnessDelta = 0.03F;
    options.minCoverage = 0.01F;
    const auto detected = clouds.detect(image, options);
    assert(detected.ok);
    assert(!detected.regions.empty());
    const auto& first = detected.regions.front();
    assert(first.x < 45.0F);
}

void testCloudDetectionDoesNotGrowBrightHazeIntoMilkyWayStructure() {
    photonstack::ImageBuffer image;
    image.width = 120;
    image.height = 80;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.045F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    // The left-hand veil is smooth and sparse enough to be a valid cloud seed.
    for (std::uint32_t y = 20; y < 60; ++y) {
        for (std::uint32_t x = 0; x < 20; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.15F;
            image.pixels[offset + 1] = 0.15F;
            image.pixels[offset + 2] = 0.16F;
        }
    }
    // An adjacent bright star field has the same low-frequency luminance, but
    // must not be filled into the cloud mask during connectivity growth.
    for (std::uint32_t y = 20; y < 60; ++y) {
        for (std::uint32_t x = 20; x < 44; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.15F;
            image.pixels[offset + 1] = 0.15F;
            image.pixels[offset + 2] = 0.16F;
        }
    }
    for (std::uint32_t y = 21; y < 60; y += 3) {
        for (std::uint32_t x = 21; x < 44; x += 3) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.92F;
            image.pixels[offset + 1] = 0.90F;
            image.pixels[offset + 2] = 0.84F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 24;
    options.rows = 16;
    options.minBrightnessDelta = 0.03F;
    options.minCoverage = 0.01F;
    const auto detected = photonstack::CloudRemoval().detect(image, options);
    assert(detected.ok);
    assert(!detected.regions.empty());
    bool masksStarField = false;
    for (const auto& region : detected.regions) {
        for (const auto& rect : region.maskRects) {
            masksStarField = masksStarField ||
                             (30.0F >= rect.x && 30.0F < rect.x + rect.width &&
                              40.0F >= rect.y && 40.0F < rect.y + rect.height);
        }
    }
    assert(!masksStarField);
}

void testCloudRemovalDoesNotAlterClearDetailInsideComponentBounds() {
    photonstack::ImageBuffer image;
    image.width = 96;
    image.height = 64;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.08F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto fillTile = [&](std::uint32_t column, std::uint32_t row, float value) {
        for (std::uint32_t y = row * 8; y < (row + 1) * 8; ++y) {
            for (std::uint32_t x = column * 8; x < (column + 1) * 8; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value;
                image.pixels[offset + 2] = value;
            }
        }
    };

    for (std::uint32_t row = 1; row <= 5; ++row) {
        fillTile(2, row, 0.22F);
    }
    for (std::uint32_t column = 2; column <= 9; ++column) {
        fillTile(column, 5, 0.22F);
    }

    // High-frequency Milky Way detail lies inside the L-shaped cloud's bounding box,
    // but outside the actual connected cloud tiles.
    for (std::uint32_t y = 16; y < 24; ++y) {
        for (std::uint32_t x = 56; x < 64; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const float value = ((x + y) % 2 == 0) ? 0.04F : 0.30F;
            image.pixels[offset] = value;
            image.pixels[offset + 1] = value;
            image.pixels[offset + 2] = value;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 12;
    options.rows = 8;
    options.minBrightnessDelta = 0.04F;
    options.minCoverage = 0.005F;
    options.strength = 0.9F;

    const photonstack::CloudRemoval clouds;
    const auto detected = clouds.detect(image, options);
    assert(detected.ok);
    assert(detected.regions.size() == 1);
    assert(detected.regions.front().width >= 64.0F);

    options.selectedIndices = {0};
    const auto removed = clouds.remove(image, options);
    assert(removed.ok);
    const auto cloudPixel = (static_cast<std::size_t>(44) * image.width + 20) * image.channels;
    const auto protectedPixel = (static_cast<std::size_t>(20) * image.width + 61) * image.channels;
    assert(removed.image.pixels[cloudPixel] < image.pixels[cloudPixel] - 0.02F);
    assert(std::fabs(removed.image.pixels[protectedPixel] - image.pixels[protectedPixel]) < 1.0e-6F);
}

void testCloudDetectionRejectsSmoothHorizonGradient() {
    photonstack::ImageBuffer image;
    image.width = 160;
    image.height = 96;
    image.channels = 4;
    image.pixels.resize(image.sampleCount());
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const float background = 0.05F + 0.18F * static_cast<float>(y) / static_cast<float>(image.height - 1);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = background;
            image.pixels[offset + 1] = background * 1.02F;
            image.pixels[offset + 2] = background * 1.08F;
            image.pixels[offset + 3] = 1.0F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 12;
    options.minBrightnessDelta = 0.03F;
    options.minCoverage = 0.01F;

    const photonstack::CloudRemoval clouds;
    const auto detected = clouds.detect(image, options);
    assert(detected.ok);
    assert(detected.regions.empty());
}

void testCloudRemovalSubtractsColoredVeilWithoutScalingStars() {
    photonstack::ImageBuffer clear;
    clear.width = 128;
    clear.height = 96;
    clear.channels = 4;
    clear.pixels.resize(clear.sampleCount());
    for (std::size_t pixel = 0; pixel < clear.pixelCount(); ++pixel) {
        const auto offset = pixel * clear.channels;
        clear.pixels[offset] = 0.04F;
        clear.pixels[offset + 1] = 0.055F;
        clear.pixels[offset + 2] = 0.085F;
        clear.pixels[offset + 3] = 1.0F;
    }

    for (std::uint32_t y = 4; y < clear.height; y += 12) {
        for (std::uint32_t x = 4; x < clear.width; x += 12) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.72F;
            clear.pixels[offset + 1] = 0.62F;
            clear.pixels[offset + 2] = 0.45F;
        }
    }

    photonstack::ImageBuffer veiled = clear;
    for (std::uint32_t y = 24; y < 72; ++y) {
        for (std::uint32_t x = 32; x < 104; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * veiled.width + x) * veiled.channels;
            veiled.pixels[offset] = std::min(1.0F, veiled.pixels[offset] + 0.12F);
            veiled.pixels[offset + 1] = std::min(1.0F, veiled.pixels[offset + 1] + 0.075F);
            veiled.pixels[offset + 2] = std::min(1.0F, veiled.pixels[offset + 2] + 0.035F);
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 16;
    options.rows = 12;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.015F;
    options.strength = 1.0F;
    options.featherRadius = 0.0F;

    const photonstack::CloudRemoval clouds;
    const auto detected = clouds.detect(veiled, options);
    assert(detected.ok);
    assert(!detected.regions.empty());
    bool starCoveredByMask = false;
    for (const auto& region : detected.regions) {
        for (const auto& rect : region.maskRects) {
            starCoveredByMask = starCoveredByMask ||
                                (52.0F >= rect.x && 52.0F < rect.x + rect.width &&
                                 40.0F >= rect.y && 40.0F < rect.y + rect.height);
        }
    }
    assert(starCoveredByMask);

    const auto removed = clouds.remove(veiled, options);
    assert(removed.ok);
    assert(removed.removedRegions >= 1);

    const auto colorError = [&](const photonstack::ImageBuffer& actual, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * actual.width + x) * actual.channels;
        return std::abs(actual.pixels[offset] - clear.pixels[offset]) +
               std::abs(actual.pixels[offset + 1] - clear.pixels[offset + 1]) +
               std::abs(actual.pixels[offset + 2] - clear.pixels[offset + 2]);
    };

    const float veiledBackgroundError = colorError(veiled, 60, 44);
    const float restoredBackgroundError = colorError(removed.image, 60, 44);
    assert(restoredBackgroundError < veiledBackgroundError * 0.35F);

    const float veiledStarError = colorError(veiled, 52, 40);
    const float restoredStarError = colorError(removed.image, 52, 40);
    assert(restoredStarError < veiledStarError * 0.35F);

    const auto clearStarOffset = (static_cast<std::size_t>(16) * clear.width + 16) * clear.channels;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        assert(std::abs(removed.image.pixels[clearStarOffset + channel] - clear.pixels[clearStarOffset + channel]) < 1.0e-6F);
    }
}

void testCloudDetectionAndRemovalWithMajorityCoverage() {
    photonstack::ImageBuffer clear;
    clear.width = 160;
    clear.height = 120;
    clear.channels = 4;
    clear.pixels.resize(clear.sampleCount());
    for (std::uint32_t y = 0; y < clear.height; ++y) {
        const float gradient = 0.015F * static_cast<float>(y) / static_cast<float>(clear.height - 1);
        for (std::uint32_t x = 0; x < clear.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.035F + gradient;
            clear.pixels[offset + 1] = 0.050F + gradient;
            clear.pixels[offset + 2] = 0.080F + gradient;
            clear.pixels[offset + 3] = 1.0F;
        }
    }
    for (std::uint32_t y = 4; y < clear.height; y += 12) {
        for (std::uint32_t x = 4; x < clear.width; x += 12) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.78F;
            clear.pixels[offset + 1] = 0.68F;
            clear.pixels[offset + 2] = 0.52F;
        }
    }

    photonstack::ImageBuffer veiled = clear;
    for (std::uint32_t y = 8; y < 112; ++y) {
        for (std::uint32_t x = 16; x < 144; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * veiled.width + x) * veiled.channels;
            veiled.pixels[offset] = std::min(1.0F, veiled.pixels[offset] + 0.14F);
            veiled.pixels[offset + 1] = std::min(1.0F, veiled.pixels[offset + 1] + 0.09F);
            veiled.pixels[offset + 2] = std::min(1.0F, veiled.pixels[offset + 2] + 0.05F);
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.01F;
    options.strength = 1.0F;
    options.featherRadius = 0.0F;

    const photonstack::CloudRemoval clouds;
    const auto detected = clouds.detect(veiled, options);
    assert(detected.ok);
    assert(!detected.regions.empty());
    assert(detected.usedMajorityFallback);
    assert(detected.regions.front().coverage > 0.50F);

    const auto removed = clouds.remove(veiled, options);
    assert(removed.ok);
    assert(removed.usedMajorityFallback);
    assert(removed.removedRegions >= 1);
    const auto colorError = [&](const photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        return std::abs(image.pixels[offset] - clear.pixels[offset]) +
               std::abs(image.pixels[offset + 1] - clear.pixels[offset + 1]) +
               std::abs(image.pixels[offset + 2] - clear.pixels[offset + 2]);
    };
    assert(colorError(removed.image, 80, 60) < colorError(veiled, 80, 60) * 0.45F);

    const auto clearOffset = (static_cast<std::size_t>(4) * clear.width + 4) * clear.channels;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        assert(std::abs(removed.image.pixels[clearOffset + channel] - clear.pixels[clearOffset + channel]) < 1.0e-6F);
    }
}

void testCloudRemovalRecoversWideVeilTouchingImageEdge() {
    photonstack::ImageBuffer clear;
    clear.width = 160;
    clear.height = 120;
    clear.channels = 4;
    clear.pixels.resize(clear.sampleCount());
    for (std::uint32_t y = 0; y < clear.height; ++y) {
        const float gradient = 0.012F * static_cast<float>(y) / static_cast<float>(clear.height - 1);
        for (std::uint32_t x = 0; x < clear.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.030F + gradient;
            clear.pixels[offset + 1] = 0.050F + gradient;
            clear.pixels[offset + 2] = 0.075F + gradient;
            clear.pixels[offset + 3] = 1.0F;
        }
    }

    photonstack::ImageBuffer veiled = clear;
    for (std::uint32_t y = 24; y < 112; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * veiled.width + x) * veiled.channels;
            veiled.pixels[offset] += 0.110F;
            veiled.pixels[offset + 1] += 0.075F;
            veiled.pixels[offset + 2] += 0.040F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.02F;
    options.minCoverage = 0.01F;
    options.strength = 1.0F;
    options.featherRadius = 0.0F;

    const photonstack::CloudRemoval clouds;
    const auto detected = clouds.detect(veiled, options);
    assert(detected.ok);
    assert(detected.usedMajorityFallback);
    assert(!detected.regions.empty());

    const auto removed = clouds.remove(veiled, options);
    assert(removed.ok);
    assert(removed.removedRegions >= 1);
    const auto colorError = [&](const photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        return std::abs(image.pixels[offset] - clear.pixels[offset]) +
               std::abs(image.pixels[offset + 1] - clear.pixels[offset + 1]) +
               std::abs(image.pixels[offset + 2] - clear.pixels[offset + 2]);
    };
    assert(colorError(removed.image, 48, 64) < colorError(veiled, 48, 64) * 0.25F);
    assert(colorError(removed.image, 150, 64) < 1.0e-6F);
}

void testCloudDetectionAndRemovalForDarkStarObscuringCloud() {
    photonstack::ImageBuffer clear;
    clear.width = 160;
    clear.height = 120;
    clear.channels = 4;
    clear.pixels.assign(clear.sampleCount(), 0.075F);
    for (std::size_t pixel = 0; pixel < clear.pixelCount(); ++pixel) {
        clear.pixels[pixel * clear.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 2; y < clear.height; y += 4) {
        for (std::uint32_t x = 2; x < clear.width; x += 4) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.80F;
            clear.pixels[offset + 1] = 0.74F;
            clear.pixels[offset + 2] = 0.62F;
        }
    }

    photonstack::ImageBuffer obscured = clear;
    for (std::uint32_t y = 64; y < 104; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * obscured.width + x) * obscured.channels;
            obscured.pixels[offset] = obscured.pixels[offset] > 0.20F ? 0.020F : 0.015F;
            obscured.pixels[offset + 1] = obscured.pixels[offset + 1] > 0.20F ? 0.020F : 0.015F;
            obscured.pixels[offset + 2] = obscured.pixels[offset + 2] > 0.20F ? 0.020F : 0.015F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.01F;
    options.strength = 1.0F;
    options.featherRadius = 0.0F;
    const photonstack::CloudRemoval clouds;
    const auto detected = clouds.detect(obscured, options);
    assert(detected.ok);
    assert(!detected.regions.empty());

    const auto removed = clouds.remove(obscured, options);
    assert(removed.ok);
    assert(removed.removedRegions >= 1);
    const auto colorError = [&](const photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        return std::abs(image.pixels[offset] - clear.pixels[offset]) +
               std::abs(image.pixels[offset + 1] - clear.pixels[offset + 1]) +
               std::abs(image.pixels[offset + 2] - clear.pixels[offset + 2]);
    };
    assert(colorError(removed.image, 20, 84) < colorError(obscured, 20, 84) * 0.15F);
    assert(colorError(removed.image, 140, 24) < 1.0e-6F);
}

void testCloudDetectionExtendsDarkEdgeCoreIntoWeakCloudFringe() {
    photonstack::ImageBuffer image;
    image.width = 160;
    image.height = 120;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.080F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 2; y < image.height; y += 4) {
        for (std::uint32_t x = 2; x < image.width; x += 4) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.86F;
            image.pixels[offset + 1] = 0.78F;
            image.pixels[offset + 2] = 0.68F;
        }
    }

    // A cloud bank enters at the lower-left corner.  Its bottom is opaque,
    // while the row just above it is a weaker fringe with fewer visible stars.
    for (std::uint32_t y = 104; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < 56; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.018F;
            image.pixels[offset + 1] = 0.019F;
            image.pixels[offset + 2] = 0.022F;
        }
    }
    for (std::uint32_t y = 96; y < 104; ++y) {
        for (std::uint32_t x = 0; x < 48; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const float attenuation = image.pixels[offset] > 0.30F ? 0.12F : 0.038F;
            image.pixels[offset] = attenuation;
            image.pixels[offset + 1] = attenuation * 1.05F;
            image.pixels[offset + 2] = attenuation * 1.16F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.005F;
    const auto detected = photonstack::CloudRemoval().detect(image, options);
    assert(detected.ok);
    assert(!detected.regions.empty());
    const auto masksPoint = [](const photonstack::CloudRegion& region, float x, float y) {
        return std::any_of(region.maskRects.begin(), region.maskRects.end(), [&](const auto& rect) {
            return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
        });
    };
    assert(std::any_of(detected.regions.begin(), detected.regions.end(), [&](const auto& region) {
        return masksPoint(region, 24.0F, 100.0F);
    }));
    assert(std::none_of(detected.regions.begin(), detected.regions.end(), [&](const auto& region) {
        return masksPoint(region, 132.0F, 108.0F);
    }));
}

void testDarkCloudDetectionSurvivesLinearExposureScaling() {
    photonstack::ImageBuffer base;
    base.width = 160;
    base.height = 120;
    base.channels = 4;
    base.colorEncoding = photonstack::ColorEncoding::Linear;
    base.pixels.assign(base.sampleCount(), 0.075F);
    for (std::size_t pixel = 0; pixel < base.pixelCount(); ++pixel) {
        base.pixels[pixel * base.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 2; y < base.height; y += 4) {
        for (std::uint32_t x = 2; x < base.width; x += 4) {
            const auto offset = (static_cast<std::size_t>(y) * base.width + x) * base.channels;
            base.pixels[offset] = 0.80F;
            base.pixels[offset + 1] = 0.74F;
            base.pixels[offset + 2] = 0.62F;
        }
    }
    for (std::uint32_t y = 64; y < 104; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * base.width + x) * base.channels;
            base.pixels[offset] = base.pixels[offset] > 0.20F ? 0.020F : 0.015F;
            base.pixels[offset + 1] = base.pixels[offset + 1] > 0.20F ? 0.020F : 0.015F;
            base.pixels[offset + 2] = base.pixels[offset + 2] > 0.20F ? 0.020F : 0.015F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.01F;
    const auto masksPoint = [](const photonstack::CloudRegion& region, float x, float y) {
        return std::any_of(region.maskRects.begin(), region.maskRects.end(), [&](const auto& rect) {
            return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
        });
    };
    for (const float scale : {0.55F, 1.0F, 1.45F}) {
        auto image = base;
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            const auto offset = pixel * image.channels;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                image.pixels[offset + channel] = std::min(1.0F, image.pixels[offset + channel] * scale);
            }
        }
        const auto detected = photonstack::CloudRemoval().detect(image, options);
        assert(detected.ok);
        assert(std::any_of(detected.regions.begin(), detected.regions.end(), [&](const auto& region) {
            return masksPoint(region, 20.0F, 84.0F);
        }));
    }
}

void testTemporalCloudDetectionConfirmsWeakDarkBankWithoutBrightStructure() {
    photonstack::ImageBuffer target;
    target.width = 160;
    target.height = 120;
    target.channels = 4;
    target.pixels.assign(target.sampleCount(), 0.075F);
    for (std::size_t pixel = 0; pixel < target.pixelCount(); ++pixel) {
        target.pixels[pixel * target.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 2; y < target.height; y += 4) {
        for (std::uint32_t x = 2; x < target.width; x += 4) {
            const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
            target.pixels[offset] = 0.80F;
            target.pixels[offset + 1] = 0.74F;
            target.pixels[offset + 2] = 0.62F;
        }
    }

    // The target frame contains a weak bank that falls below the normal
    // single-frame edge threshold; later frames show the same bank opaque.
    for (std::uint32_t y = 64; y < 104; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
            target.pixels[offset] = 0.055F;
            target.pixels[offset + 1] = 0.055F;
            target.pixels[offset + 2] = 0.055F;
        }
    }
    std::vector<photonstack::ImageBuffer> references;
    for (int frame = 0; frame < 3; ++frame) {
        auto reference = target;
        for (std::uint32_t y = 64; y < 104; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * reference.width + x) * reference.channels;
                reference.pixels[offset] = 0.015F;
                reference.pixels[offset + 1] = reference.pixels[offset];
                reference.pixels[offset + 2] = reference.pixels[offset];
            }
        }
        references.push_back(std::move(reference));
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.045F;
    options.minCoverage = 0.01F;
    const auto single = photonstack::CloudRemoval().detect(target, options);
    assert(single.ok);
    const auto temporal = photonstack::CloudRemoval().detectTemporal(target, references, options);
    assert(temporal.ok);
    assert(temporal.temporalFramesUsed == 4);
    assert(temporal.temporallyConfirmedRegions >= 1);
    const auto weakBank = std::find_if(temporal.regions.begin(), temporal.regions.end(), [](const auto& region) {
        return region.x < 40.0F && region.y >= 56.0F && region.meanLuminance < region.backgroundLuminance &&
               region.temporalSupport >= 2;
    });
    assert(weakBank != temporal.regions.end());

    auto weakTemporalOptions = options;
    weakTemporalOptions.minBrightnessDelta = 0.015F;
    std::vector<photonstack::CloudRemovalResult> streamedReferences;
    for (const auto& reference : references) {
        streamedReferences.push_back(photonstack::CloudRemoval().detect(reference, weakTemporalOptions));
        assert(streamedReferences.back().ok);
    }
    const auto streamed = photonstack::CloudRemoval().detectTemporalFromDetections(target, streamedReferences, options);
    assert(streamed.ok);
    assert(streamed.regions.size() == temporal.regions.size());
    assert(streamed.temporallyConfirmedRegions == temporal.temporallyConfirmedRegions);

    auto removalOptions = options;
    removalOptions.selectedIndices = {weakBank->index};
    const auto removed = photonstack::CloudRemoval().removeTemporal(target, references, removalOptions);
    assert(removed.ok);
    assert(removed.removedRegions == 1);
    const auto removedOffset = (static_cast<std::size_t>(80) * target.width + 12) * target.channels;
    assert(removed.image.pixels[removedOffset] > target.pixels[removedOffset]);
    const auto streamedRemoval = photonstack::CloudRemoval().removeTemporalFromDetections(
        target,
        streamedReferences,
        removalOptions
    );
    assert(streamedRemoval.ok);
    assert(streamedRemoval.removedRegions == 1);
    assert(streamedRemoval.image.pixels[removedOffset] > target.pixels[removedOffset]);
}

void testCloudMajorityFallbackRejectsDarkObstructionCross() {
    photonstack::ImageBuffer image;
    image.width = 160;
    image.height = 120;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.08F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
    for (std::uint32_t y = 48; y < 72; ++y) {
        for (std::uint32_t x = 64; x < 96; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 0.015F;
            image.pixels[offset + 1] = 0.018F;
            image.pixels[offset + 2] = 0.022F;
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.01F;

    const auto detected = photonstack::CloudRemoval().detect(image, options);
    assert(detected.ok);
    assert(!detected.usedMajorityFallback);
    assert(detected.regions.empty());
}

void testGridBackgroundExtraction() {
    photonstack::ImageBuffer image;
    image.width = 4;
    image.height = 2;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);

    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const float value = x < 2 ? 0.2F : 0.6F;
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = value;
            image.pixels[offset + 1] = value;
            image.pixels[offset + 2] = value;
            image.pixels[offset + 3] = 1.0F;
        }
    }

    const photonstack::BackgroundExtractor extractor;
    const auto result = extractor.extractGrid(
        image, {.extraction = {.mode = photonstack::BackgroundMode::Subtract}, .columns = 2, .rows = 1});

    assert(result.ok);
    assert(result.backgroundGrid.size() == 2);
    assert(std::fabs(result.backgroundGrid[0] - 0.2F) < 0.001F);
    assert(std::fabs(result.backgroundGrid[1] - 0.6F) < 0.001F);
    assert(std::fabs(result.image.pixels[3] - 1.0F) < 0.001F);
}

void testTranslationRegistration() {
    photonstack::ImageBuffer reference;
    reference.width = 8;
    reference.height = 8;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(reference, 3, 3, 1.0F);
    setPixel(moving, 4, 3, 1.0F);

    const photonstack::Registration registration;
    const auto result = registration.estimateTranslation(
        reference, moving, {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 2.0F});

    assert(result.ok);
    assert(result.matches == 1);
    assert(std::fabs(result.translation.dx + 1.0F) < 0.1F);
    assert(std::fabs(result.translation.dy - 0.0F) < 0.1F);

    const auto aligned = registration.applyTranslation(moving, result.translation);
    const auto alignedOffset = (static_cast<std::size_t>(3) * aligned.width + 3) * aligned.channels;
    assert(aligned.pixels[alignedOffset] > 0.9F);
}

void testTranslationRegistrationFindsLargeOffset() {
    photonstack::ImageBuffer reference;
    reference.width = 20;
    reference.height = 12;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(reference, 10, 3, 1.0F);
    setPixel(reference, 14, 6, 0.9F);
    setPixel(reference, 9, 9, 0.8F);
    setPixel(moving, 2, 3, 1.0F);
    setPixel(moving, 6, 6, 0.9F);
    setPixel(moving, 1, 9, 0.8F);

    const photonstack::Registration registration;
    const auto result = registration.estimateTranslation(
        reference, moving, {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 2.0F});

    assert(result.ok);
    assert(result.matches >= 3);
    assert(std::fabs(result.translation.dx - 8.0F) < 0.1F);
    assert(std::fabs(result.translation.dy) < 0.1F);
}

void testAlignedAverageStack() {
    photonstack::ImageBuffer reference;
    reference.width = 8;
    reference.height = 8;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(reference, 3, 3, 1.0F);
    setPixel(moving, 4, 3, 1.0F);

    const auto referencePath = std::filesystem::temp_directory_path() / "photonstack-align-reference.png";
    const auto movingPath = std::filesystem::temp_directory_path() / "photonstack-align-moving.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, reference, referencePath).ok);
    assert(writeSRGBFixture(codec, moving, movingPath).ok);

    const photonstack::Stacker stacker;
    std::vector<photonstack::StackProgress> progressEvents;
    photonstack::StackOptions alignedOptions = {
        .method = photonstack::StackMethod::Average,
        .alignTranslation = true,
        .registration = {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 2.0F},
    };
    alignedOptions.progress = [&](const photonstack::StackProgress& progress) {
        progressEvents.push_back(progress);
    };
    const auto result = stacker.stack({referencePath, movingPath}, alignedOptions);

    assert(result.ok);
    assert(std::any_of(progressEvents.begin(), progressEvents.end(), [](const auto& progress) {
        return progress.stage == photonstack::StackProgressStage::Aligning;
    }));
    const auto alignedOffset = (static_cast<std::size_t>(3) * result.image.width + 3) * result.image.channels;
    assert(result.image.pixels[alignedOffset] > 0.9F);

    const auto cachesBefore = stackCacheDirectories();
    const auto weightedResult = stacker.stack(
        {referencePath, movingPath},
        {.method = photonstack::StackMethod::WeightedAverage,
         .alignTranslation = true,
         .registration = {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 2.0F}});
    assert(weightedResult.ok);
    assert(weightedResult.alignedFrames == 1);
    assert(weightedResult.image.pixels[alignedOffset] > 0.9F);
    assert(stackCacheDirectories() == cachesBefore);
}

void testSimilarityRegistration() {
    photonstack::ImageBuffer reference;
    reference.width = 12;
    reference.height = 12;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(reference, 3, 3, 1.0F);
    setPixel(reference, 8, 3, 0.9F);
    setPixel(reference, 6, 8, 0.8F);
    setPixel(moving, 4, 3, 1.0F);
    setPixel(moving, 9, 3, 0.9F);
    setPixel(moving, 7, 8, 0.8F);

    const photonstack::Registration registration;
    const auto result = registration.estimateSimilarity(
        reference, moving, {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 2.0F});

    assert(result.ok);
    assert(result.matches == 3);
    assert(std::fabs(result.transform.dx + 1.0F) < 0.1F);
    assert(std::fabs(result.transform.scale - 1.0F) < 0.1F);

    const auto aligned = registration.applySimilarity(moving, result.transform);
    const auto alignedOffset = (static_cast<std::size_t>(3) * aligned.width + 3) * aligned.channels;
    assert(aligned.pixels[alignedOffset] > 0.9F);
}

void testRegistrationResamplingIgnoresColorBehindTransparencyBeforeStacking() {
    photonstack::ImageBuffer source;
    source.width = 2;
    source.height = 1;
    source.channels = 4;
    source.colorEncoding = photonstack::ColorEncoding::Linear;
    source.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
    };

    const photonstack::Registration registration;
    const auto aligned = registration.applySimilarity(
        source,
        {.scale = 1.0F, .rotationRadians = 0.0F, .dx = 0.5F, .dy = 0.0F}
    );
    assert(!aligned.empty());
    constexpr std::size_t edgeOffset = 4;
    assert(std::fabs(aligned.pixels[edgeOffset] - 1.0F) < 1.0e-6F);
    assert(std::fabs(aligned.pixels[edgeOffset + 1]) < 1.0e-6F);
    assert(std::fabs(aligned.pixels[edgeOffset + 2]) < 1.0e-6F);
    assert(std::fabs(aligned.pixels[edgeOffset + 3] - 0.5F) < 1.0e-6F);

    photonstack::ImageBuffer reference = source;
    reference.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 0.0F, 0.0F, 1.0F,
    };
    const auto referencePath =
        std::filesystem::temp_directory_path() / "photonstack-alpha-aware-reference.fits";
    const auto alignedPath =
        std::filesystem::temp_directory_path() / "photonstack-alpha-aware-aligned.fits";
    const photonstack::ImageCodec codec;
    assert(codec.write(reference, referencePath).ok);
    assert(codec.write(aligned, alignedPath).ok);

    const auto stacked = photonstack::Stacker().average({referencePath, alignedPath});
    assert(stacked.ok);
    assert(std::fabs(stacked.image.pixels[edgeOffset] - 1.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[edgeOffset + 1]) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[edgeOffset + 2]) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[edgeOffset + 3] - 0.75F) < 1.0e-6F);

    std::error_code error;
    std::filesystem::remove(referencePath, error);
    std::filesystem::remove(alignedPath, error);
}

void testSimilarityRegistrationCorrectsFieldRotation() {
    photonstack::ImageBuffer reference;
    reference.width = 160;
    reference.height = 120;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);
    const float centerX = 80.0F;
    const float centerY = 60.0F;
    const float rotation = 3.0F * 3.14159265358979323846F / 180.0F;
    const float cosTheta = std::cos(rotation);
    const float sinTheta = std::sin(rotation);
    const float shiftX = 3.5F;
    const float shiftY = -2.25F;
    const std::vector<std::pair<float, float>> stars = {
        {34.0F, 27.0F},
        {68.0F, 22.0F},
        {119.0F, 30.0F},
        {45.0F, 64.0F},
        {92.0F, 55.0F},
        {132.0F, 81.0F},
        {63.0F, 96.0F},
        {104.0F, 101.0F},
    };

    float peak = 1.0F;
    for (const auto& [movingX, movingY] : stars) {
        addGaussianStar(moving, movingX, movingY, peak);
        const float localX = movingX - centerX;
        const float localY = movingY - centerY;
        const float referenceX = centerX + cosTheta * localX - sinTheta * localY + shiftX;
        const float referenceY = centerY + sinTheta * localX + cosTheta * localY + shiftY;
        addGaussianStar(reference, referenceX, referenceY, peak);
        peak -= 0.06F;
    }

    const photonstack::Registration registration;
    const auto result = registration.estimateSimilarity(
        reference,
        moving,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 40}, .matchTolerance = 6.0F, .minimumMatches = 6});

    assert(result.ok);
    assert(!result.usedFallback);
    assert(result.matches >= 6);
    assert(std::fabs(result.transform.rotationRadians - rotation) < 0.01F);

    const auto aligned = registration.applySimilarity(moving, result.transform);
    const photonstack::StarDetector detector;
    const auto referenceStars = detector.detect(reference, {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 40});
    const auto alignedStars = detector.detect(aligned, {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 40});
    assert(referenceStars.ok);
    assert(alignedStars.ok);
    assert(referenceStars.stars.size() >= 6);
    assert(alignedStars.stars.size() >= 6);

    float totalDistance = 0.0F;
    for (const auto& referenceStar : referenceStars.stars) {
        float bestDistance = 1000.0F;
        for (const auto& alignedStar : alignedStars.stars) {
            const float dx = referenceStar.x - alignedStar.x;
            const float dy = referenceStar.y - alignedStar.y;
            bestDistance = std::min(bestDistance, std::sqrt(dx * dx + dy * dy));
        }
        totalDistance += bestDistance;
    }
    assert(totalDistance / static_cast<float>(referenceStars.stars.size()) < 1.25F);
}

void testSimilarityStackKeepsDenseRotatedStarFieldSharp() {
    constexpr std::uint32_t width = 1200;
    constexpr std::uint32_t height = 800;
    const float centerX = static_cast<float>(width) * 0.5F;
    const float centerY = static_cast<float>(height) * 0.5F;
    std::vector<std::pair<float, float>> stars;
    stars.reserve(120);
    std::uint32_t state = 0x9E3779B9U;
    while (stars.size() < 120) {
        state = state * 1664525U + 1013904223U;
        const float x = 90.0F + static_cast<float>(state % 1020U);
        state = state * 1664525U + 1013904223U;
        const float y = 90.0F + static_cast<float>(state % 620U);
        bool separated = true;
        for (const auto& [existingX, existingY] : stars) {
            const float dx = existingX - x;
            const float dy = existingY - y;
            if (dx * dx + dy * dy < 14.0F * 14.0F) {
                separated = false;
                break;
            }
        }
        if (separated) {
            stars.push_back({x, y});
        }
    }

    std::vector<std::filesystem::path> paths;
    const photonstack::ImageCodec codec;
    for (std::size_t frame = 0; frame < 5; ++frame) {
        photonstack::ImageBuffer image;
        image.width = width;
        image.height = height;
        image.channels = 4;
        image.pixels.assign(image.sampleCount(), 0.0F);

        const float angle = static_cast<float>(frame) * 0.95F * 3.14159265358979323846F / 180.0F;
        const float cosTheta = std::cos(angle);
        const float sinTheta = std::sin(angle);
        const float shiftX = static_cast<float>(frame) * 3.25F;
        const float shiftY = static_cast<float>(frame) * -2.75F;
        float peak = 1.0F;
        for (const auto& [x, y] : stars) {
            const float localX = x - centerX;
            const float localY = y - centerY;
            const float transformedX = centerX + cosTheta * localX - sinTheta * localY + shiftX;
            const float transformedY = centerY + sinTheta * localX + cosTheta * localY + shiftY;
            addGaussianStar(image, transformedX, transformedY, peak);
            peak = peak <= 0.55F ? 1.0F : peak - 0.025F;
        }

        const auto path = std::filesystem::temp_directory_path() / ("photonstack-dense-rotated-" + std::to_string(frame) + ".png");
        assert(writeSRGBFixture(codec, image, path).ok);
        paths.push_back(path);
    }

    const photonstack::Stacker stacker;
    const auto result = stacker.stack(
        paths,
        {.method = photonstack::StackMethod::Average,
         .alignSimilarity = true,
         .registration = {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 600}, .matchTolerance = 3.0F, .minimumMatches = 20}});

    assert(result.ok);
    assert(result.alignedFrames == 4);
    assert(result.alignmentFallbacks == 0);
    assert(result.minimumAlignmentMatches >= 40);

    const photonstack::StarDetector detector;
    const auto detected = detector.detect(result.image, {.sigmaThreshold = 3.0F, .minPeak = 0.05F, .maxStars = 1000});
    assert(detected.ok);
    assert(detected.stars.size() >= 100);
    assert(detected.stars.size() <= 160);
}

void testDistortionStackCorrectsLocalStarFieldWarp() {
    constexpr std::uint32_t width = 1200;
    constexpr std::uint32_t height = 800;
    const float centerX = static_cast<float>(width) * 0.5F;
    const float centerY = static_cast<float>(height) * 0.5F;
    std::vector<std::pair<float, float>> stars;
    stars.reserve(110);
    std::uint32_t state = 0xA511E9B3U;
    while (stars.size() < 110) {
        state = state * 1664525U + 1013904223U;
        const float x = 100.0F + static_cast<float>(state % 1000U);
        state = state * 1664525U + 1013904223U;
        const float y = 90.0F + static_cast<float>(state % 620U);
        bool separated = true;
        for (const auto& [existingX, existingY] : stars) {
            const float dx = existingX - x;
            const float dy = existingY - y;
            if (dx * dx + dy * dy < 16.0F * 16.0F) {
                separated = false;
                break;
            }
        }
        if (separated) {
            stars.push_back({x, y});
        }
    }

    std::vector<std::filesystem::path> paths;
    const photonstack::ImageCodec codec;
    for (std::size_t frame = 0; frame < 5; ++frame) {
        photonstack::ImageBuffer image;
        image.width = width;
        image.height = height;
        image.channels = 4;
        image.pixels.assign(image.sampleCount(), 0.0F);

        const float angle = static_cast<float>(frame) * 0.7F * 3.14159265358979323846F / 180.0F;
        const float cosTheta = std::cos(angle);
        const float sinTheta = std::sin(angle);
        const float shiftX = static_cast<float>(frame) * 2.25F;
        const float shiftY = static_cast<float>(frame) * -1.75F;
        const float radialCoefficient = static_cast<float>(frame) * 6.0e-8F;
        float peak = 1.0F;
        for (const auto& [x, y] : stars) {
            const float localX = x - centerX;
            const float localY = y - centerY;
            const float rotatedX = cosTheta * localX - sinTheta * localY;
            const float rotatedY = sinTheta * localX + cosTheta * localY;
            const float radiusSquared = rotatedX * rotatedX + rotatedY * rotatedY;
            const float warpedX = centerX + rotatedX + rotatedX * radialCoefficient * radiusSquared + shiftX;
            const float warpedY = centerY + rotatedY + rotatedY * radialCoefficient * radiusSquared + shiftY;
            addGaussianStar(image, warpedX, warpedY, peak);
            peak = peak <= 0.55F ? 1.0F : peak - 0.03F;
        }

        const auto path = std::filesystem::temp_directory_path() / ("photonstack-distorted-field-" + std::to_string(frame) + ".png");
        assert(writeSRGBFixture(codec, image, path).ok);
        paths.push_back(path);
    }

    const photonstack::Stacker stacker;
    const auto result = stacker.stack(
        paths,
        {.method = photonstack::StackMethod::Average,
         .alignDistortion = true,
         .registration = {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 600}, .matchTolerance = 3.0F, .minimumMatches = 20}});

    assert(result.ok);
    assert(result.alignedFrames == 4);
    assert(result.alignmentFallbacks == 0);
    assert(result.minimumAlignmentMatches >= 40);

    const photonstack::StarDetector detector;
    const auto detected = detector.detect(result.image, {.sigmaThreshold = 3.0F, .minPeak = 0.05F, .maxStars = 1000});
    assert(detected.ok);
    assert(detected.stars.size() >= 90);
    assert(detected.stars.size() <= 170);
    const auto elongated = std::count_if(detected.stars.begin(), detected.stars.end(), [](const photonstack::Star& star) {
        return star.eccentricity > 0.75F;
    });
    assert(elongated <= 8);

    const auto robustResult = stacker.stack(
        paths,
        {.method = photonstack::StackMethod::Median,
         .alignDistortion = true,
         .registration = {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 600}, .matchTolerance = 3.0F, .minimumMatches = 20}});
    assert(robustResult.ok);
    assert(robustResult.alignedFrames == 4);
    assert(robustResult.alignmentFallbacks == 0);
    assert(robustResult.minimumAlignmentMatches >= 40);
    const auto robustDetected =
        detector.detect(robustResult.image, {.sigmaThreshold = 3.0F, .minPeak = 0.05F, .maxStars = 1000});
    assert(robustDetected.ok);
    assert(robustDetected.stars.size() >= 90);
    assert(robustDetected.stars.size() <= 170);
}

void testDistortionRegistrationUsesProjectiveBaselineForWideFieldWarp() {
    constexpr std::uint32_t width = 1000;
    constexpr std::uint32_t height = 700;
    photonstack::ImageBuffer reference;
    reference.width = width;
    reference.height = height;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);
    std::uint32_t state = 0x6A09E667U;
    for (std::uint32_t y = 70; y < height - 70; y += 58) {
        for (std::uint32_t x = 75; x < width - 75; x += 64) {
            state = state * 1664525U + 1013904223U;
            const float jitterX = static_cast<float>(static_cast<int>(state % 17U) - 8) * 0.45F;
            state = state * 1664525U + 1013904223U;
            const float jitterY = static_cast<float>(static_cast<int>(state % 17U) - 8) * 0.45F;
            const float movingX = static_cast<float>(x) + jitterX;
            const float movingY = static_cast<float>(y) + jitterY;
            const float denominator = 1.0F + 1.1e-5F * movingX - 8.0e-6F * movingY;
            const float referenceX = (1.001F * movingX + 0.020F * movingY - 13.0F) / denominator;
            const float referenceY = (-0.014F * movingX + 0.998F * movingY + 16.0F) / denominator;
            if (referenceX < 10.0F || referenceY < 10.0F || referenceX > static_cast<float>(width - 11) ||
                referenceY > static_cast<float>(height - 11)) {
                continue;
            }
            const float peak = 0.65F + static_cast<float>((x + y) % 7U) * 0.05F;
            addGaussianStar(moving, movingX, movingY, peak);
            addGaussianStar(reference, referenceX, referenceY, peak);
        }
    }

    const photonstack::Registration registration;
    photonstack::DistortionTransform transform;
    const photonstack::RegistrationOptions options = {
        .starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 900},
        .matchTolerance = 3.0F,
        .minimumMatches = 30,
    };
    const auto result = registration.estimateDistortion(reference, moving, transform, options);
    assert(result.ok);
    assert(transform.useProjective);
    assert(result.matches >= 90);

    const auto aligned = registration.applyDistortion(moving, transform);
    const auto residual = registration.estimateTranslation(
        reference, aligned,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 900},
         .matchTolerance = 2.0F,
         .minimumMatches = 30});
    assert(residual.ok);
    assert(std::fabs(residual.translation.dx) < 0.35F);
    assert(std::fabs(residual.translation.dy) < 0.35F);
    assert(residual.matches >= 90);
}

void testSimilarityRegistrationFallsBackToTranslation() {
    photonstack::ImageBuffer reference;
    reference.width = 10;
    reference.height = 10;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(reference, 4, 4, 1.0F);
    setPixel(moving, 6, 4, 1.0F);

    const photonstack::Registration registration;
    const auto result = registration.estimateSimilarity(
        reference,
        moving,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 3.0F, .minimumMatches = 3});

    assert(result.ok);
    assert(result.usedFallback);
    assert(std::fabs(result.translation.dx + 2.0F) < 0.1F);
    assert(std::fabs(result.transform.rotationRadians) < 0.001F);
}

void testAffineRegistrationCorrectsEdgeShear() {
    photonstack::ImageBuffer reference;
    reference.width = 80;
    reference.height = 70;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);

    photonstack::ImageBuffer moving = reference;
    makeOpaque(reference);
    makeOpaque(moving);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };
    const auto mapX = [](float x, float y) {
        return x + y * 0.2F + 2.0F;
    };
    const auto mapY = [](float x, float y) {
        return x * 0.1F + y - 3.0F;
    };

    const std::vector<std::pair<std::uint32_t, std::uint32_t>> stars = {
        {10, 10},
        {50, 10},
        {15, 35},
        {55, 45},
        {35, 55},
    };
    float value = 1.0F;
    for (const auto& [x, y] : stars) {
        setPixel(moving, x, y, value);
        setPixel(reference,
                 static_cast<std::uint32_t>(std::lround(mapX(static_cast<float>(x), static_cast<float>(y)))),
                 static_cast<std::uint32_t>(std::lround(mapY(static_cast<float>(x), static_cast<float>(y)))),
                 value);
        value -= 0.08F;
    }

    const photonstack::Registration registration;
    const auto result = registration.estimateAffine(
        reference,
        moving,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 3.0F, .minimumMatches = 3});

    assert(result.ok);
    assert(result.matches >= 3);
    assert(std::fabs(result.affine.a - 1.0F) < 0.04F);
    assert(std::fabs(result.affine.b - 0.2F) < 0.04F);
    assert(std::fabs(result.affine.c - 0.1F) < 0.04F);
    assert(std::fabs(result.affine.d - 1.0F) < 0.04F);

    photonstack::ImageBuffer single = moving;
    single.pixels.assign(single.sampleCount(), 0.0F);
    setPixel(single, 10, 10, 1.0F);
    const auto aligned =
        registration.applyAffine(single, {.a = 1.0F, .b = 0.0F, .c = 0.0F, .d = 1.0F, .dx = 4.0F, .dy = -2.0F});
    const auto alignedOffset = (static_cast<std::size_t>(8) * aligned.width + 14) * aligned.channels;
    assert(aligned.pixels[alignedOffset] > 0.9F);
}

void testAffineRegistrationPopulatesSimilarityFallbackMatrix() {
    photonstack::ImageBuffer reference;
    reference.width = 120;
    reference.height = 80;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);
    photonstack::ImageBuffer moving = reference;

    addGaussianStar(reference, 30.0F, 25.0F, 1.0F);
    addGaussianStar(reference, 82.0F, 57.0F, 0.8F);
    addGaussianStar(moving, 34.0F, 22.0F, 1.0F);
    addGaussianStar(moving, 86.0F, 54.0F, 0.8F);

    const photonstack::Registration registration;
    const auto result = registration.estimateAffine(
        reference, moving,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 20},
         .matchTolerance = 2.0F,
         .minimumMatches = 2});

    assert(result.ok);
    assert(result.usedFallback);
    assert(std::fabs(result.affine.dx + 4.0F) < 0.35F);
    assert(std::fabs(result.affine.dy - 3.0F) < 0.35F);
    const auto aligned = registration.applyAffine(moving, result.affine);
    const auto residual = registration.estimateTranslation(
        reference, aligned,
        {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 20},
         .matchTolerance = 2.0F,
         .minimumMatches = 2});
    assert(residual.ok);
    assert(std::fabs(residual.translation.dx) < 0.35F);
    assert(std::fabs(residual.translation.dy) < 0.35F);
}

void testRegistrationRowRenderingMatchesFullFrames() {
    photonstack::ImageBuffer image;
    image.width = 17;
    image.height = 13;
    image.channels = 4;
    image.pixels.resize(image.sampleCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        image.pixels[offset] = static_cast<float>((pixel * 3) % 97) / 96.0F;
        image.pixels[offset + 1] = static_cast<float>((pixel * 5) % 89) / 88.0F;
        image.pixels[offset + 2] = static_cast<float>((pixel * 7) % 83) / 82.0F;
        image.pixels[offset + 3] = pixel % 11 == 0 ? 0.0F : 1.0F;
    }

    const photonstack::Registration registration;
    const auto verifyRows = [&](const photonstack::ImageBuffer& expected, const auto& render) {
        std::vector<float> rendered(expected.sampleCount(), -1.0F);
        std::uint32_t rowCount = 0;
        const bool ok = render([&](std::uint32_t row, const float* samples, std::size_t sampleCount) {
            assert(row == rowCount);
            assert(sampleCount == static_cast<std::size_t>(expected.width) * expected.channels);
            const auto offset = static_cast<std::size_t>(row) * sampleCount;
            std::copy(samples, samples + sampleCount, rendered.begin() + static_cast<std::ptrdiff_t>(offset));
            ++rowCount;
            return true;
        });
        assert(ok);
        assert(rowCount == expected.height);
        assert(rendered == expected.pixels);
    };

    const photonstack::Translation translation = {.dx = 2.0F, .dy = -1.0F};
    verifyRows(registration.applyTranslation(image, translation),
               [&](const auto& consumer) { return registration.renderTranslationRows(image, translation, consumer); });

    const photonstack::SimilarityTransform similarity = {
        .scale = 1.015F,
        .rotationRadians = 0.025F,
        .dx = 1.25F,
        .dy = -0.75F,
    };
    verifyRows(registration.applySimilarity(image, similarity),
               [&](const auto& consumer) { return registration.renderSimilarityRows(image, similarity, consumer); });

    const photonstack::AffineTransform affine = {
        .a = 1.01F,
        .b = 0.015F,
        .c = -0.01F,
        .d = 0.995F,
        .dx = 0.5F,
        .dy = -1.25F,
    };
    verifyRows(registration.applyAffine(image, affine),
               [&](const auto& consumer) { return registration.renderAffineRows(image, affine, consumer); });

    photonstack::DistortionTransform distortion;
    distortion.global = similarity;
    distortion.influenceRadius = 72.0F;
    distortion.controlPoints = {
        {.x = 2.0F, .y = 2.0F, .dx = 0.10F, .dy = -0.08F}, {.x = 8.0F, .y = 2.0F, .dx = -0.05F, .dy = 0.06F},
        {.x = 14.0F, .y = 3.0F, .dx = 0.08F, .dy = 0.03F}, {.x = 3.0F, .y = 10.0F, .dx = -0.06F, .dy = -0.04F},
        {.x = 9.0F, .y = 9.0F, .dx = 0.03F, .dy = 0.07F},  {.x = 15.0F, .y = 11.0F, .dx = -0.02F, .dy = -0.05F},
    };
    verifyRows(registration.applyDistortion(image, distortion),
               [&](const auto& consumer) { return registration.renderDistortionRows(image, distortion, consumer); });

    std::uint32_t cancelledRows = 0;
    const bool completed = registration.renderTranslationRows(
        image, translation, [&](std::uint32_t, const float*, std::size_t) { return ++cancelledRows < 3; });
    assert(!completed);
    assert(cancelledRows == 3);
}

void testDistortionForwardCoordinatesMatchReverseSampling() {
    photonstack::ImageBuffer coordinateImage;
    coordinateImage.width = 160;
    coordinateImage.height = 120;
    coordinateImage.channels = 4;
    coordinateImage.pixels.resize(coordinateImage.sampleCount());
    for (std::uint32_t y = 0; y < coordinateImage.height; ++y) {
        for (std::uint32_t x = 0; x < coordinateImage.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * coordinateImage.width + x) * 4;
            coordinateImage.pixels[offset] = static_cast<float>(x) / 159.0F;
            coordinateImage.pixels[offset + 1] = static_cast<float>(y) / 119.0F;
            coordinateImage.pixels[offset + 2] = 0.0F;
            coordinateImage.pixels[offset + 3] = 1.0F;
        }
    }

    photonstack::DistortionTransform transform;
    transform.global = {.scale = 1.012F, .rotationRadians = 0.018F, .dx = 2.5F, .dy = -1.75F};
    transform.usePolynomial = true;
    transform.polynomialDx[3] = 0.55F;
    transform.polynomialDx[5] = -0.35F;
    transform.polynomialDy[3] = -0.25F;
    transform.polynomialDy[4] = 0.40F;
    transform.influenceRadius = 72.0F;
    transform.controlPoints = {
        {.x = 20.0F, .y = 20.0F, .dx = 0.25F, .dy = -0.20F},
        {.x = 80.0F, .y = 18.0F, .dx = -0.15F, .dy = 0.18F},
        {.x = 140.0F, .y = 25.0F, .dx = 0.20F, .dy = 0.12F},
        {.x = 25.0F, .y = 95.0F, .dx = -0.18F, .dy = -0.16F},
        {.x = 82.0F, .y = 92.0F, .dx = 0.12F, .dy = 0.22F},
        {.x = 138.0F, .y = 98.0F, .dx = -0.10F, .dy = -0.18F},
    };

    const photonstack::Registration registration;
    const auto aligned = registration.applyDistortion(coordinateImage, transform);
    std::size_t checked = 0;
    const bool mapped = registration.renderDistortionForwardRows(
        coordinateImage, transform,
        [&](std::uint32_t row, const float* coordinates, std::size_t coordinateCount) {
            assert(coordinateCount == static_cast<std::size_t>(coordinateImage.width) * 2);
            if (row != 22 && row != 58 && row != 94) {
                return true;
            }
            for (const std::uint32_t sourceX : {24U, 78U, 134U}) {
                const auto coordinateOffset = static_cast<std::size_t>(sourceX) * 2;
                const int targetX = static_cast<int>(std::lround(coordinates[coordinateOffset]));
                const int targetY = static_cast<int>(std::lround(coordinates[coordinateOffset + 1]));
                assert(targetX >= 0 && targetX < static_cast<int>(aligned.width));
                assert(targetY >= 0 && targetY < static_cast<int>(aligned.height));
                const auto outputOffset =
                    (static_cast<std::size_t>(targetY) * aligned.width + static_cast<std::uint32_t>(targetX)) * 4;
                const float sampledX = aligned.pixels[outputOffset] * 159.0F;
                const float sampledY = aligned.pixels[outputOffset + 1] * 119.0F;
                assert(std::fabs(sampledX - static_cast<float>(sourceX)) < 0.85F);
                assert(std::fabs(sampledY - static_cast<float>(row)) < 0.85F);
                ++checked;
            }
            return true;
        });
    assert(mapped);
    assert(checked == 9);
}

void testStarMask() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 5;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);
    makeOpaque(image);

    const auto offset = (static_cast<std::size_t>(2) * image.width + 2) * image.channels;
    image.pixels[offset] = 1.0F;
    image.pixels[offset + 1] = 1.0F;
    image.pixels[offset + 2] = 1.0F;
    image.pixels[offset + 3] = 1.0F;

    const photonstack::StarMask starMask;
    const auto result = starMask.create(image, {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1});

    assert(result.ok);
    assert(result.stars == 1);
    assert(result.mask.pixels[offset] > 0.9F);
    assert(result.mask.format == photonstack::PixelFormat::Float32RGBA);
    assert(result.mask.colorEncoding == photonstack::ColorEncoding::Unknown);
    for (std::size_t pixel = 0; pixel < result.mask.pixelCount(); ++pixel) {
        assert(result.mask.pixels[pixel * result.mask.channels + 3] == 1.0F);
    }
    assert(result.mask.pixels[3] == 1.0F);
    assert(result.mask.pixels[0] == 0.0F);

    const auto maskPath = std::filesystem::temp_directory_path() / "photonstack-star-mask-scalar-roundtrip.png";
    const photonstack::ImageCodec codec;
    assert(codec.write(result.mask, maskPath).ok);
    const auto decodedMask = codec.read(maskPath);
    assert(decodedMask.ok);
    const auto featherOffset = (static_cast<std::size_t>(2) * image.width + 3) * result.mask.channels;
    assert(std::fabs(decodedMask.image.pixels[featherOffset] - result.mask.pixels[featherOffset]) < 0.01F);
    std::error_code maskCleanupError;
    std::filesystem::remove(maskPath, maskCleanupError);

    const auto layered = starMask.create(image, {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F},
                                                 .radius = 1,
                                                 .largeRadius = 2,
                                                 .largeStarPeak = 0.8F,
                                                 .layered = true});
    assert(layered.ok);
    assert(layered.largeStars == 1);

    photonstack::ImageBuffer clippedField;
    clippedField.width = 120;
    clippedField.height = 20;
    clippedField.channels = 4;
    clippedField.pixels.assign(clippedField.sampleCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < clippedField.pixelCount(); ++pixel) {
        clippedField.pixels[pixel * clippedField.channels + 3] = 1.0F;
    }
    for (std::uint32_t index = 0; index < 20; ++index) {
        const auto x = 4U + index * 5U;
        const float shoulder = 0.05F + static_cast<float>(index) * 0.02F;
        setRgbaPixel(clippedField, x, 10, 1.0F);
        setRgbaPixel(clippedField, x - 1, 10, shoulder);
        setRgbaPixel(clippedField, x + 1, 10, shoulder);
        setRgbaPixel(clippedField, x, 9, shoulder);
        setRgbaPixel(clippedField, x, 11, shoulder);
    }
    const auto clippedLayered = starMask.create(
        clippedField,
        {.detection = {.sigmaThreshold = 1.5F, .minPeak = 0.1F, .maxStars = 100},
         .radius = 1,
         .largeRadius = 3,
         .largeStarPeak = 0.8F,
         .layered = true});
    assert(clippedLayered.ok);
    assert(clippedLayered.stars == 20);
    assert(clippedLayered.largeStars > 0);
    assert(clippedLayered.largeStars < clippedLayered.stars / 2);
}

void testStarReduction() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 5;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.1F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto centerOffset = (static_cast<std::size_t>(2) * image.width + 2) * image.channels;
    image.pixels[centerOffset] = 1.0F;
    image.pixels[centerOffset + 1] = 1.0F;
    image.pixels[centerOffset + 2] = 1.0F;
    const auto maskedOffset = (static_cast<std::size_t>(2) * image.width + 3) * image.channels;
    image.pixels[maskedOffset] = 0.0F;
    image.pixels[maskedOffset + 1] = 0.0F;
    image.pixels[maskedOffset + 2] = 0.0F;
    image.pixels[maskedOffset + 3] = 0.0F;

    const photonstack::StarReducer reducer;
    const auto result = reducer.reduce(
        image,
        {.mask = {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1}, .amount = 1.0F, .radius = 1});

    assert(result.ok);
    assert(result.stars == 1);
    assert(result.image.pixels[centerOffset] < 0.2F);
    assert(result.image.pixels[centerOffset] > 0.09F);
    assert(result.image.pixels[maskedOffset] == 0.0F);
    assert(result.image.pixels[maskedOffset + 3] == 0.0F);
    assert(std::fabs(result.image.pixels[0] - 0.1F) < 0.001F);

    auto partialBoundary = image;
    partialBoundary.pixels[maskedOffset + 3] = 0.01F;
    const auto partialBoundaryResult = reducer.reduce(
        partialBoundary,
        {.mask = {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1},
         .amount = 1.0F,
         .radius = 1}
    );
    assert(partialBoundaryResult.ok);
    assert(std::fabs(partialBoundaryResult.image.pixels[centerOffset] - result.image.pixels[centerOffset]) < 0.01F);

    auto visibleInvalid = image;
    visibleInvalid.pixels[maskedOffset] = std::numeric_limits<float>::quiet_NaN();
    visibleInvalid.pixels[maskedOffset + 3] = 1.0F;
    const auto invalidSource = reducer.reduce(visibleInvalid);
    assert(!invalidSource.ok);
    assert(invalidSource.errorCode == "ImageBufferInvalid");

    const auto profileAware = reducer.reduce(
        image, {.mask = {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1, .layered = true},
                .amount = 1.0F,
                .radius = 1,
                .profileAware = true});
    assert(profileAware.ok);
    assert(profileAware.image.pixels[centerOffset] > result.image.pixels[centerOffset]);

    photonstack::ImageBuffer rgbImage;
    rgbImage.width = image.width;
    rgbImage.height = image.height;
    rgbImage.channels = 3;
    rgbImage.format = photonstack::PixelFormat::Float32RGBA;
    rgbImage.pixels.resize(rgbImage.sampleCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            rgbImage.pixels[pixel * rgbImage.channels + channel] = image.pixels[pixel * image.channels + channel];
        }
    }
    const auto rgbResult = reducer.reduce(
        rgbImage,
        {.mask = {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1},
         .amount = 1.0F,
         .radius = 1}
    );
    assert(rgbResult.ok);
    assert(rgbResult.stars == 1);
    const auto rgbCenterOffset = (static_cast<std::size_t>(2) * rgbImage.width + 2) * rgbImage.channels;
    assert(rgbResult.image.pixels[rgbCenterOffset] < 0.2F);

    photonstack::ImageBuffer scientific = image;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto scientificOffset = pixel * scientific.channels;
        if (scientific.pixels[scientificOffset + 3] <= 0.0F) {
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            scientific.pixels[scientificOffset + channel] *= 0.01F;
        }
    }
    const auto displayReference = reducer.reduce(
        image,
        {.mask = {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1, .layered = true},
         .amount = 1.0F,
         .radius = 1,
         .profileAware = true}
    );
    const auto scientificResult = reducer.reduceUsingDetectionImage(
        scientific,
        image,
        {.mask = {.detection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .radius = 1, .layered = true},
         .amount = 1.0F,
         .radius = 1,
         .profileAware = true}
    );
    assert(displayReference.ok);
    assert(scientificResult.ok);
    assert(scientificResult.stars == displayReference.stars);
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto scientificOffset = pixel * scientific.channels;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            assert(std::fabs(
                scientificResult.image.pixels[scientificOffset + channel] -
                displayReference.image.pixels[scientificOffset + channel] * 0.01F
            ) < 1.0e-6F);
        }
        assert(scientificResult.image.pixels[scientificOffset + 3] ==
               displayReference.image.pixels[scientificOffset + 3]);
    }

    auto mismatchedDetection = image;
    mismatchedDetection.width -= 1;
    mismatchedDetection.pixels.resize(mismatchedDetection.sampleCount());
    const auto mismatch = reducer.reduceUsingDetectionImage(scientific, mismatchedDetection);
    assert(!mismatch.ok);
    assert(mismatch.errorCode == "ImageDimensionsMismatch");
}

void testComaReductionSuppressesElongatedStarTail() {
    photonstack::ImageBuffer image;
    image.width = 80;
    image.height = 60;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.04F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const float centerX = 40.0F;
    const float centerY = 30.0F;
    for (int y = 18; y <= 42; ++y) {
        for (int x = 24; x <= 56; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float value = 0.04F + 0.92F * std::exp(-(dx * dx / (2.0F * 4.0F * 4.0F) +
                                                           dy * dy / (2.0F * 1.2F * 1.2F)));
            const auto offset = (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels;
            image.pixels[offset] = std::max(image.pixels[offset], value);
            image.pixels[offset + 1] = std::max(image.pixels[offset + 1], value);
            image.pixels[offset + 2] = std::max(image.pixels[offset + 2], value);
        }
    }

    const auto centerOffset = (static_cast<std::size_t>(30) * image.width + 40) * image.channels;
    const auto tailOffset = (static_cast<std::size_t>(30) * image.width + 48) * image.channels;
    const auto maskedOffset = (static_cast<std::size_t>(5) * image.width + 5) * image.channels;
    image.pixels[maskedOffset] = 100.0F;
    image.pixels[maskedOffset + 1] = 100.0F;
    image.pixels[maskedOffset + 2] = 100.0F;
    image.pixels[maskedOffset + 3] = 0.0F;
    const float originalCenter = image.pixels[centerOffset];
    const float originalTail = image.pixels[tailOffset];

    const photonstack::ComaReducer reducer;
    const auto result = reducer.reduce(
        image,
        {.detection = {.sigmaThreshold = 1.8F, .minPeak = 0.08F, .maxStars = 20},
         .amount = 1.0F,
         .radius = 10,
         .eccentricityThreshold = 0.25F,
         .edgeAware = false});

    assert(result.ok);
    assert(result.correctedStars >= 1);
    assert(result.image.pixels[tailOffset] < originalTail - 0.01F);
    assert(result.image.pixels[centerOffset] > originalCenter * 0.90F);
    assert(std::fabs(result.image.pixels[0] - 0.04F) < 0.001F);
    assert(result.image.pixels[maskedOffset] == 0.0F);
    assert(result.image.pixels[maskedOffset + 3] == 0.0F);

    auto partialShape = image;
    const auto partialShapeOffset = (static_cast<std::size_t>(31) * image.width + 51) * image.channels;
    partialShape.pixels[partialShapeOffset] = 1.0F;
    partialShape.pixels[partialShapeOffset + 1] = 1.0F;
    partialShape.pixels[partialShapeOffset + 2] = 1.0F;
    partialShape.pixels[partialShapeOffset + 3] = 0.01F;
    auto transparentShape = partialShape;
    transparentShape.pixels[partialShapeOffset + 3] = 0.0F;
    const auto partialShapeResult = reducer.reduce(
        partialShape,
        {.detection = {.sigmaThreshold = 1.8F, .minPeak = 0.08F, .maxStars = 20},
         .amount = 1.0F,
         .radius = 10,
         .eccentricityThreshold = 0.25F,
         .edgeAware = false}
    );
    const auto transparentShapeResult = reducer.reduce(
        transparentShape,
        {.detection = {.sigmaThreshold = 1.8F, .minPeak = 0.08F, .maxStars = 20},
         .amount = 1.0F,
         .radius = 10,
         .eccentricityThreshold = 0.25F,
         .edgeAware = false}
    );
    assert(partialShapeResult.ok);
    assert(transparentShapeResult.ok);
    assert(std::fabs(partialShapeResult.averageEccentricity - transparentShapeResult.averageEccentricity) < 0.02F);
    assert(std::fabs(partialShapeResult.image.pixels[tailOffset] -
                     transparentShapeResult.image.pixels[tailOffset]) < 0.02F);

    auto invalidComa = image;
    invalidComa.pixels[maskedOffset] = std::numeric_limits<float>::infinity();
    invalidComa.pixels[maskedOffset + 3] = 1.0F;
    const auto invalidComaResult = reducer.reduce(invalidComa);
    assert(!invalidComaResult.ok);
    assert(invalidComaResult.errorCode == "ImageBufferInvalid");

    photonstack::ImageBuffer scientific = image;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        const float value = image.pixels[offset];
        scientific.pixels[offset] = value * -0.002F;
        scientific.pixels[offset + 1] = value * 0.012F;
        scientific.pixels[offset + 2] = value * 0.008F;
    }
    const auto scientificResult = reducer.reduceUsingDetectionImage(
        scientific,
        image,
        {.detection = {.sigmaThreshold = 1.8F, .minPeak = 0.08F, .maxStars = 20},
         .amount = 1.0F,
         .radius = 10,
         .eccentricityThreshold = 0.25F,
         .edgeAware = false}
    );
    assert(scientificResult.ok);
    assert(scientificResult.stars == result.stars);
    assert(scientificResult.correctedStars == result.correctedStars);
    const float displayTailScale = result.image.pixels[tailOffset] / image.pixels[tailOffset];
    for (std::uint16_t channel = 0; channel < 3; ++channel) {
        const auto offset = tailOffset + channel;
        const float scientificTailScale = scientificResult.image.pixels[offset] / scientific.pixels[offset];
        assert(std::fabs(scientificTailScale - displayTailScale) < 0.0005F);
    }
    assert(scientificResult.image.pixels[tailOffset] > scientific.pixels[tailOffset]);
    assert(scientificResult.image.pixels[tailOffset + 1] < scientific.pixels[tailOffset + 1]);
    assert(scientificResult.image.pixels[tailOffset + 3] == scientific.pixels[tailOffset + 3]);

    auto mismatchedDetection = image;
    --mismatchedDetection.width;
    mismatchedDetection.pixels.resize(mismatchedDetection.sampleCount());
    const auto mismatch = reducer.reduceUsingDetectionImage(scientific, mismatchedDetection);
    assert(!mismatch.ok);
    assert(mismatch.errorCode == "ImageDimensionsMismatch");
}

void testComaReductionHonorsDetectionLimit() {
    photonstack::ImageBuffer image;
    image.width = 100;
    image.height = 80;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.01F);
    makeOpaque(image);
    const std::vector<std::pair<float, float>> stars = {
        {15.0F, 15.0F},
        {35.0F, 15.0F},
        {55.0F, 15.0F},
        {75.0F, 15.0F},
        {20.0F, 45.0F},
        {45.0F, 50.0F},
        {75.0F, 55.0F},
    };
    float peak = 1.0F;
    for (const auto& [x, y] : stars) {
        addGaussianStar(image, x, y, peak);
        peak -= 0.05F;
    }

    const auto result = photonstack::ComaReducer().reduce(
        image,
        {.detection = {.sigmaThreshold = 1.5F, .minPeak = 0.05F, .maxStars = 3},
         .amount = 0.0F,
         .radius = 4,
         .eccentricityThreshold = 0.2F}
    );
    assert(result.ok);
    assert(result.stars == 3);
}

void testArtifactTrailRemovalPreservesMeteors() {
    photonstack::ImageBuffer image;
    image.width = 64;
    image.height = 32;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setPixel = [](photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value;
        target.pixels[offset + 2] = value;
    };

    for (std::uint32_t x = 8; x <= 56; ++x) {
        setPixel(image, x, 16, 0.85F);
    }

    const photonstack::ArtifactTrailRemover remover;
    std::vector<photonstack::ArtifactTrailProgress> removalProgress;
    photonstack::ArtifactTrailOptions removalOptions = {
        .sigmaThreshold = 2.0F,
        .minPeak = 0.1F,
        .minLength = 10.0F,
        .airplaneLength = 30.0F,
        .maxWidth = 4.0F,
    };
    removalOptions.progress = [&](const photonstack::ArtifactTrailProgress& progress) {
        removalProgress.push_back(progress);
    };
    const auto removed = remover.remove(image, removalOptions);

    assert(removed.ok);
    assert(!removalProgress.empty());
    assert(removalProgress.front().stage == photonstack::ArtifactTrailProgressStage::Analyzing);
    assert(removalProgress.back().stage == photonstack::ArtifactTrailProgressStage::Cleaning);
    assert(std::fabs(removalProgress.back().progress - 1.0) < 1.0e-9);
    assert(std::is_sorted(removalProgress.begin(), removalProgress.end(), [](const auto& left, const auto& right) {
        return left.progress < right.progress;
    }));
    assert(std::any_of(removalProgress.begin(), removalProgress.end(), [](const auto& progress) {
        return progress.stage == photonstack::ArtifactTrailProgressStage::Masking;
    }));
    assert(std::any_of(removalProgress.begin(), removalProgress.end(), [](const auto& progress) {
        return progress.stage == photonstack::ArtifactTrailProgressStage::Inpainting;
    }));
    assert(removed.removedTrails == 1);
    assert(!removed.trails.empty());
    assert(removed.trails.front().kind == photonstack::ArtifactTrailKind::Satellite);
    assert(removed.trails.front().meanBrightness > 0.0F);
    assert(removed.trails.front().weight > 0.0F);
    const auto center = (static_cast<std::size_t>(16) * image.width + 32) * image.channels;
    assert(removed.image.pixels[center] < 0.2F);

    photonstack::ImageBuffer meteor = image;
    meteor.pixels.assign(meteor.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < meteor.pixelCount(); ++pixel) {
        meteor.pixels[pixel * meteor.channels + 3] = 1.0F;
    }
    for (std::uint32_t x = 8; x <= 56; ++x) {
        const float t = static_cast<float>(x - 8) / 48.0F;
        setPixel(meteor, x, 12, 0.9F - t * 0.65F);
    }

    photonstack::ArtifactTrailOptions preserveMeteorOptions;
    preserveMeteorOptions.sigmaThreshold = 2.0F;
    preserveMeteorOptions.minPeak = 0.1F;
    preserveMeteorOptions.minLength = 10.0F;
    preserveMeteorOptions.airplaneLength = 30.0F;
    preserveMeteorOptions.maxWidth = 4.0F;
    preserveMeteorOptions.includeMeteors = true;
    const auto preserved = remover.remove(meteor, preserveMeteorOptions);
    const auto meteorHead = (static_cast<std::size_t>(12) * meteor.width + 8) * meteor.channels;
    assert(preserved.ok);
    assert(preserved.removedTrails == 0);
    assert(preserved.protectedMeteors == 1);
    assert(!preserved.trails.empty());
    assert(preserved.trails.front().kind == photonstack::ArtifactTrailKind::Meteor);
    assert(preserved.trails.front().weight > 0.0F);
    assert(preserved.image.pixels[meteorHead] > 0.8F);

    photonstack::ArtifactTrailOptions manualMeteorOptions;
    manualMeteorOptions.sigmaThreshold = 2.0F;
    manualMeteorOptions.minPeak = 0.1F;
    manualMeteorOptions.minLength = 10.0F;
    manualMeteorOptions.airplaneLength = 30.0F;
    manualMeteorOptions.maxWidth = 4.0F;
    manualMeteorOptions.preserveMeteors = false;
    manualMeteorOptions.removeAirplanes = false;
    manualMeteorOptions.removeDrones = false;
    manualMeteorOptions.removeMeteors = true;
    manualMeteorOptions.selectedIndices = {0};
    manualMeteorOptions.includeMeteors = true;
    const auto manuallyRemovedMeteor = remover.remove(meteor, manualMeteorOptions);
    assert(manuallyRemovedMeteor.ok);
    assert(manuallyRemovedMeteor.removedTrails == 1);
    assert(manuallyRemovedMeteor.image.pixels[meteorHead] < 0.2F);
}

void testArtifactTrailClassifiesAndSelectivelyRemovesContinuousSatellite() {
    photonstack::ImageBuffer image;
    image.width = 240;
    image.height = 120;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.04F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setNeutralPixel = [](photonstack::ImageBuffer& target,
                                    std::uint32_t x,
                                    std::uint32_t y,
                                    float value) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value;
        target.pixels[offset + 2] = value;
    };
    for (std::uint32_t x = 28; x <= 212; ++x) {
        const auto y = static_cast<std::uint32_t>(50 + std::lround(static_cast<float>(x - 28) * 0.07F));
        setNeutralPixel(image, x, y, 0.52F);
        setNeutralPixel(image, x, y + 1, 0.31F);
    }
    for (std::uint32_t x = 18; x < 225; x += 29) {
        setNeutralPixel(image, x, 18 + (x % 23), 0.22F);
    }

    photonstack::ArtifactTrailOptions options = {
        .sigmaThreshold = 2.0F,
        .minPeak = 0.08F,
        .minLength = 12.0F,
        .airplaneLength = 48.0F,
        .maxWidth = 5.0F,
    };
    const photonstack::ArtifactTrailRemover remover;
    const auto detected = remover.detect(image, options);
    assert(detected.ok);
    const auto satellite = std::find_if(detected.trails.begin(), detected.trails.end(), [](const auto& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Satellite && trail.length > 170.0F;
    });
    assert(satellite != detected.trails.end());

    options.removeAirplanes = false;
    options.removeDrones = false;
    options.removeSatellites = true;
    const auto removed = remover.remove(image, options);
    assert(removed.ok);
    assert(removed.removedTrails == 1);
    const auto center = (static_cast<std::size_t>(56) * image.width + 120) * image.channels;
    assert(removed.image.pixels[center] < image.pixels[center] - 0.12F);

    options.detectedTrails = detected.trails;
    options.useDetectedTrails = true;
    const auto removedUsingDetectionReport = remover.remove(image, options);
    assert(removedUsingDetectionReport.ok);
    assert(removedUsingDetectionReport.removedTrails == 1);
    assert(removedUsingDetectionReport.image.pixels[center] < image.pixels[center] - 0.12F);

    options.removeSatellites = false;
    const auto preserved = remover.remove(image, options);
    assert(preserved.ok);
    assert(preserved.removedTrails == 0);
    assert(std::fabs(preserved.image.pixels[center] - image.pixels[center]) < 0.0001F);
}

void testArtifactTrailRecoversCenteredSymmetricSatelliteFlare() {
    photonstack::ImageBuffer image;
    image.width = 800;
    image.height = 600;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.020F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    constexpr float angle = 0.22F;
    const float unitX = std::cos(angle);
    const float unitY = std::sin(angle);
    const float normalX = -unitY;
    const float normalY = unitX;
    for (float distance = -115.0F; distance <= 115.0F; distance += 1.0F) {
        const float centered = std::fabs(distance) / 115.0F;
        const float value = 0.22F + (1.0F - centered) * 0.68F;
        for (float normalDistance = -1.0F; normalDistance <= 1.0F; normalDistance += 1.0F) {
            const auto x = static_cast<std::uint32_t>(std::lround(
                400.0F + unitX * distance + normalX * normalDistance
            ));
            const auto y = static_cast<std::uint32_t>(std::lround(
                300.0F + unitY * distance + normalY * normalDistance
            ));
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = value;
            image.pixels[offset + 1] = value;
            image.pixels[offset + 2] = value;
        }
    }

    photonstack::ArtifactTrailOptions options;
    options.includeMeteors = true;
    const auto result = photonstack::ArtifactTrailRemover().detect(image, options);
    assert(result.ok);
    const auto flare = std::find_if(result.trails.begin(), result.trails.end(), [](const auto& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Satellite &&
               trail.verifiedContinuousSatellite && trail.length >= 180.0F &&
               trail.peakPosition >= 0.38F && trail.peakPosition <= 0.62F;
    });
    assert(flare != result.trails.end());
}

void testArtifactTrailDetectionExcludesMeteorsByDefault() {
    photonstack::ImageBuffer image;
    image.width = 64;
    image.height = 32;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    for (std::uint32_t x = 8; x <= 56; ++x) {
        const float t = static_cast<float>(x - 8) / 48.0F;
        const auto offset = (static_cast<std::size_t>(12) * image.width + x) * image.channels;
        image.pixels[offset] = 0.9F - t * 0.65F;
        image.pixels[offset + 1] = 0.9F - t * 0.65F;
        image.pixels[offset + 2] = 0.9F - t * 0.65F;
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto artifactOnly = remover.detect(
        image,
        {.sigmaThreshold = 2.0F, .minPeak = 0.1F, .minLength = 10.0F, .airplaneLength = 30.0F, .maxWidth = 4.0F});
    assert(artifactOnly.ok);
    assert(std::none_of(artifactOnly.trails.begin(), artifactOnly.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Meteor;
    }));

    photonstack::ArtifactTrailOptions meteorOptions;
    meteorOptions.sigmaThreshold = 2.0F;
    meteorOptions.minPeak = 0.1F;
    meteorOptions.minLength = 10.0F;
    meteorOptions.airplaneLength = 30.0F;
    meteorOptions.maxWidth = 4.0F;
    meteorOptions.includeMeteors = true;
    const auto meteorAware = remover.detect(image, meteorOptions);
    assert(meteorAware.ok);
    assert(std::any_of(meteorAware.trails.begin(), meteorAware.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Meteor;
    }));
}

void testArtifactTrailExtendsFaintMeteorTail() {
    photonstack::ImageBuffer image;
    image.width = 160;
    image.height = 90;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.045F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setPixel = [](photonstack::ImageBuffer& target, std::int32_t x, std::int32_t y, float value) {
        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(target.width) ||
            y >= static_cast<std::int32_t>(target.height)) {
            return;
        }
        const auto offset = (static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * target.width +
                             static_cast<std::uint32_t>(x)) *
                            target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value;
        target.pixels[offset + 2] = value;
    };

    for (std::uint32_t i = 0; i <= 104; ++i) {
        const float t = static_cast<float>(i) / 104.0F;
        const auto x = static_cast<std::int32_t>(std::round(26.0F + t * 104.0F));
        const auto y = static_cast<std::int32_t>(std::round(66.0F - t * 30.0F));
        const float value = 0.88F - t * 0.68F;
        setPixel(image, x, y, value);
        if (i % 2 == 0) {
            setPixel(image, x, y + 1, value * 0.45F);
        }
    }

    const photonstack::ArtifactTrailRemover remover;
    photonstack::ArtifactTrailOptions options;
    options.sigmaThreshold = 2.2F;
    options.minPeak = 0.12F;
    options.minLength = 8.0F;
    options.airplaneLength = 70.0F;
    options.maxWidth = 5.0F;
    options.includeMeteors = true;
    const auto result = remover.detect(image, options);

    assert(result.ok);
    const auto meteor = std::find_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Meteor && trail.length > 85.0F;
    });
    assert(meteor != result.trails.end());
    assert(meteor->confidence > 0.8F);
}

void testArtifactTrailDetectsDroneLikeShortTrack() {
    photonstack::ImageBuffer image;
    image.width = 40;
    image.height = 24;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.03F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
    for (std::uint32_t x = 12; x <= 28; ++x) {
        const auto offset = (static_cast<std::size_t>(10) * image.width + x) * image.channels;
        image.pixels[offset] = 0.7F;
        image.pixels[offset + 1] = 0.7F;
        image.pixels[offset + 2] = 0.7F;
    }

    const photonstack::ArtifactTrailRemover remover;
    std::vector<photonstack::ArtifactTrailProgress> detectionProgress;
    photonstack::ArtifactTrailOptions detectionOptions = {
        .sigmaThreshold = 2.0F,
        .minPeak = 0.1F,
        .minLength = 8.0F,
        .airplaneLength = 30.0F,
        .maxWidth = 4.0F,
    };
    detectionOptions.progress = [&](const photonstack::ArtifactTrailProgress& progress) {
        detectionProgress.push_back(progress);
    };
    const auto result = remover.detect(image, detectionOptions);

    assert(result.ok);
    assert(!detectionProgress.empty());
    assert(detectionProgress.front().stage == photonstack::ArtifactTrailProgressStage::Analyzing);
    assert(detectionProgress.back().stage == photonstack::ArtifactTrailProgressStage::Refining);
    assert(std::fabs(detectionProgress.back().progress - 1.0) < 1.0e-9);
    assert(std::is_sorted(detectionProgress.begin(), detectionProgress.end(), [](const auto& left, const auto& right) {
        return left.progress < right.progress;
    }));
    assert(std::any_of(detectionProgress.begin(), detectionProgress.end(), [](const auto& progress) {
        return progress.stage == photonstack::ArtifactTrailProgressStage::PatternSearch;
    }));
    assert(result.trails.size() == 1);
    assert(result.trails.front().kind == photonstack::ArtifactTrailKind::Drone);
}

void testArtifactTrailRejectsDenseRandomStarAlignments() {
    photonstack::ImageBuffer image;
    image.width = 256;
    image.height = 256;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.018F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    std::uint32_t state = 0x9e3779b9U;
    const auto nextRandom = [&]() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    for (std::size_t index = 0; index < 950; ++index) {
        const std::uint32_t x = 2 + nextRandom() % (image.width - 4);
        const std::uint32_t y = 2 + nextRandom() % (image.height - 4);
        const float brightness = 0.10F + static_cast<float>(nextRandom() % 520U) / 1000.0F;
        const float redScale = 0.78F + static_cast<float>(nextRandom() % 220U) / 1000.0F;
        const float greenScale = 0.80F + static_cast<float>(nextRandom() % 200U) / 1000.0F;
        const float blueScale = 0.82F + static_cast<float>(nextRandom() % 180U) / 1000.0F;
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = brightness * redScale;
        image.pixels[offset + 1] = brightness * greenScale;
        image.pixels[offset + 2] = brightness * blueScale;
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(result.trails.empty());
}

void testArtifactTrailPreservesRotationalStarTrailField() {
    photonstack::ImageBuffer image;
    image.width = 768;
    image.height = 512;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.018F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto paint = [&](std::int32_t x, std::int32_t y, float value) {
        if (x < 1 || y < 1 || x >= static_cast<std::int32_t>(image.width - 1) ||
            y >= static_cast<std::int32_t>(image.height - 1)) {
            return;
        }
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto offset =
                    (static_cast<std::size_t>(y + dy) * image.width + static_cast<std::uint32_t>(x + dx)) *
                    image.channels;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value * 0.96F;
                image.pixels[offset + 2] = value * 0.90F;
            }
        }
    };
    constexpr float pi = 3.14159265358979323846F;
    const float rotationCenterX = 376.0F;
    const float rotationCenterY = 236.0F;
    for (std::size_t index = 0; index < 48; ++index) {
        const float angle = static_cast<float>(index) * (2.0F * pi / 48.0F) +
                            static_cast<float>(index % 3) * 0.025F;
        const float radius = 105.0F + static_cast<float>((index * 47) % 245);
        const float centerX = rotationCenterX + std::cos(angle) * radius;
        const float centerY = rotationCenterY + std::sin(angle) * radius;
        const float tangentX = -std::sin(angle);
        const float tangentY = std::cos(angle);
        const float tangentLength = std::max(1.0F, std::hypot(tangentX, tangentY));
        const float unitX = tangentX / tangentLength;
        const float unitY = tangentY / tangentLength;
        const float halfLength = 18.0F + static_cast<float>(index % 5) * 3.0F;
        for (float distance = -halfLength; distance <= halfLength; distance += 1.0F) {
            paint(static_cast<std::int32_t>(std::lround(centerX + unitX * distance)),
                  static_cast<std::int32_t>(std::lround(centerY + unitY * distance)),
                  0.62F + static_cast<float>(index % 4) * 0.06F);
        }
    }
    // A rotating star-trail exposure often includes bright terrain, roads, or
    // observatory lights near the horizon. These straight foreground edges
    // must not be treated as removable aircraft merely because the sky also
    // contains many line-like components.
    for (std::size_t index = 0; index < 6; ++index) {
        const float startX = 48.0F + static_cast<float>(index) * 72.0F;
        const float startY = 390.0F + static_cast<float>(index) * 16.0F;
        const float slope = (static_cast<float>(index % 3) - 1.0F) * 0.08F;
        for (float distance = 0.0F; distance <= 230.0F; distance += 1.0F) {
            paint(static_cast<std::int32_t>(std::lround(startX + distance)),
                  static_cast<std::int32_t>(std::lround(startY + distance * slope)),
                  0.24F + static_cast<float>(index % 2) * 0.05F);
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(result.trails.empty());
}

void testArtifactTrailPreservesFarOutsideRotationalStarTrailField() {
    photonstack::ImageBuffer image;
    image.width = 900;
    image.height = 600;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.020F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto paint = [&](float x, float y, float value) {
        const auto centerX = static_cast<std::int32_t>(std::lround(x));
        const auto centerY = static_cast<std::int32_t>(std::lround(y));
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto sampleX = centerX + dx;
                const auto sampleY = centerY + dy;
                if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<std::int32_t>(image.width) ||
                    sampleY >= static_cast<std::int32_t>(image.height)) {
                    continue;
                }
                const auto offset =
                    (static_cast<std::size_t>(sampleY) * image.width + static_cast<std::uint32_t>(sampleX)) *
                    image.channels;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value * 0.98F;
                image.pixels[offset + 2] = value * 0.94F;
            }
        }
    };

    const float poleX = 450.0F;
    const float poleY = 1650.0F;
    for (std::size_t row = 0; row < 5; ++row) {
        for (std::size_t column = 0; column < 7; ++column) {
            const float centerX = 65.0F + static_cast<float>(column) * 128.0F;
            const float centerY = 58.0F + static_cast<float>(row) * 116.0F;
            const float radialX = centerX - poleX;
            const float radialY = centerY - poleY;
            const float radialLength = std::hypot(radialX, radialY);
            const float unitX = -radialY / radialLength;
            const float unitY = radialX / radialLength;
            const float halfLength = 34.0F + static_cast<float>((row + column) % 3) * 5.0F;
            for (float distance = -halfLength; distance <= halfLength; distance += 1.0F) {
                paint(centerX + unitX * distance,
                      centerY + unitY * distance,
                      0.66F + static_cast<float>((row + column) % 4) * 0.05F);
            }
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(result.trails.empty());
}

void testArtifactTrailFindsLocallyContrastedContinuousSatellite() {
    photonstack::ImageBuffer image;
    image.width = 800;
    image.height = 600;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.075F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const float background = y >= 500 ? 0.78F : 0.075F + static_cast<float>(x) / 80000.0F;
            image.pixels[offset] = background;
            image.pixels[offset + 1] = background;
            image.pixels[offset + 2] = background * 1.01F;
            image.pixels[offset + 3] = 1.0F;
        }
    }
    for (std::uint32_t y = 80; y <= 430; ++y) {
        const std::uint32_t x = 330 + (y - 80) / 14;
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const auto sampleX = static_cast<std::uint32_t>(static_cast<std::int32_t>(x) + dx);
            const auto offset = (static_cast<std::size_t>(y) * image.width + sampleX) * image.channels;
            image.pixels[offset] = 0.155F;
            image.pixels[offset + 1] = 0.157F;
            image.pixels[offset + 2] = 0.160F;
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(std::any_of(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Satellite && trail.length >= 300.0F &&
               trail.verifiedContinuousSatellite;
    }));
}

void testArtifactTrailFindsNeutralPeriodicSatelliteChain() {
    photonstack::ImageBuffer image;
    image.width = 800;
    image.height = 600;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.025F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    std::uint32_t state = 0x5f12a8c3U;
    const auto nextRandom = [&]() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    for (std::size_t index = 0; index < 180; ++index) {
        const auto x = 8U + nextRandom() % (image.width - 16U);
        const auto y = 8U + nextRandom() % (image.height - 16U);
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        const float value = 0.10F + static_cast<float>(nextRandom() % 260U) / 1000.0F;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
    }

    for (std::size_t index = 0; index < 10; ++index) {
        const auto centerX = static_cast<std::int32_t>(150 + index * 22);
        const auto centerY = static_cast<std::int32_t>(185 + index * 9);
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto x = centerX + dx;
                const auto y = centerY + dy;
                const auto offset =
                    (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels;
                const float value = dx == 0 && dy == 0 ? 0.64F : 0.42F;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value;
                image.pixels[offset + 2] = value;
            }
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(std::any_of(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Satellite &&
               trail.verifiedSegmentedSatelliteChain && trail.path.size() >= 8 && trail.length >= 170.0F;
    }));
}

void testArtifactTrailRejectsChromaticPeriodicTexture() {
    photonstack::ImageBuffer image;
    image.width = 800;
    image.height = 600;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.025F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    for (std::size_t index = 0; index < 10; ++index) {
        const auto centerX = static_cast<std::int32_t>(145 + index * 23);
        const auto centerY = static_cast<std::int32_t>(320 + index * 8);
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto x = centerX + dx;
                const auto y = centerY + dy;
                const auto offset =
                    (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels;
                image.pixels[offset] = dx == 0 && dy == 0 ? 0.72F : 0.46F;
                image.pixels[offset + 1] = dx == 0 && dy == 0 ? 0.48F : 0.31F;
                image.pixels[offset + 2] = dx == 0 && dy == 0 ? 0.34F : 0.22F;
            }
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(std::none_of(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.verifiedSegmentedSatelliteChain;
    }));
}

void testArtifactTrailRejectsSaturatedPeriodicStars() {
    photonstack::ImageBuffer image;
    image.width = 800;
    image.height = 600;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.025F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    for (std::size_t index = 0; index < 10; ++index) {
        const auto centerX = static_cast<std::int32_t>(12 + index * 12);
        const auto centerY = static_cast<std::int32_t>(210 + index * 21);
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto x = centerX + dx;
                const auto y = centerY + dy;
                const auto offset =
                    (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels;
                const float value = dx == 0 && dy == 0 ? 0.99F : 0.92F;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value;
                image.pixels[offset + 2] = value;
            }
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(std::none_of(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.verifiedSegmentedSatelliteChain;
    }));
}

void testArtifactTrailPreservesTwoPieceColorfulMeteor() {
    photonstack::ImageBuffer image;
    image.width = 640;
    image.height = 480;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.018F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto paint = [&](float x, float y, float value) {
        const auto centerX = static_cast<std::int32_t>(std::lround(x));
        const auto centerY = static_cast<std::int32_t>(std::lround(y));
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto sampleX = centerX + dx;
                const auto sampleY = centerY + dy;
                if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<std::int32_t>(image.width) ||
                    sampleY >= static_cast<std::int32_t>(image.height)) {
                    continue;
                }
                const auto offset =
                    (static_cast<std::size_t>(sampleY) * image.width + static_cast<std::uint32_t>(sampleX)) *
                    image.channels;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value * 0.42F;
                image.pixels[offset + 2] = value * 0.20F;
            }
        }
    };
    constexpr float angle = 0.66F;
    const float unitX = std::cos(angle);
    const float unitY = std::sin(angle);
    for (float distance = -92.0F; distance <= -30.0F; distance += 1.0F) {
        paint(315.0F + unitX * distance, 215.0F + unitY * distance, 0.72F);
    }
    for (float distance = -20.0F; distance <= 55.0F; distance += 1.0F) {
        paint(315.0F + unitX * distance, 215.0F + unitY * distance, 0.58F);
    }

    photonstack::ArtifactTrailOptions options;
    options.sigmaThreshold = 2.0F;
    options.minPeak = 0.08F;
    options.minLength = 12.0F;
    options.airplaneLength = 180.0F;
    options.maxWidth = 8.0F;
    const auto result = photonstack::ArtifactTrailRemover().detect(image, options);
    assert(result.ok);
    assert(result.trails.empty());
    assert(result.protectedMeteors == 2);
}

void testArtifactTrailPreservesLongBrightChromaticMeteor() {
    photonstack::ImageBuffer image;
    image.width = 640;
    image.height = 480;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.012F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    constexpr float angle = 0.88F;
    const float unitX = std::cos(angle);
    const float unitY = std::sin(angle);
    const float normalX = -unitY;
    const float normalY = unitX;
    for (float distance = -92.0F; distance <= 92.0F; distance += 1.0F) {
        const float position = (distance + 92.0F) / 184.0F;
        for (float normalDistance = -2.0F; normalDistance <= 2.0F; normalDistance += 1.0F) {
            const auto x = static_cast<std::int32_t>(std::lround(
                320.0F + unitX * distance + normalX * normalDistance
            ));
            const auto y = static_cast<std::int32_t>(std::lround(
                220.0F + unitY * distance + normalY * normalDistance
            ));
            const auto offset =
                (static_cast<std::size_t>(y) * image.width + static_cast<std::uint32_t>(x)) * image.channels;
            image.pixels[offset] = 0.95F;
            image.pixels[offset + 1] = 0.82F - position * 0.28F;
            image.pixels[offset + 2] = 0.42F + position * 0.42F;
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(result.trails.empty());
    assert(result.protectedMeteors >= 1);
}

void testArtifactTrailRejectsNoisyDenseStarField() {
    photonstack::ImageBuffer image;
    image.width = 640;
    image.height = 480;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);

    std::uint32_t state = 0x71b34c29U;
    const auto nextRandom = [&]() {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const float noise = static_cast<float>(nextRandom() & 0xFFFFU) / 65535.0F;
        const float redNoise = static_cast<float>(nextRandom() & 0xFFU) / 255.0F;
        const float blueNoise = static_cast<float>(nextRandom() & 0xFFU) / 255.0F;
        const auto offset = pixel * image.channels;
        image.pixels[offset] = 0.012F + noise * 0.050F + redNoise * 0.008F;
        image.pixels[offset + 1] = 0.012F + noise * 0.045F;
        image.pixels[offset + 2] = 0.012F + noise * 0.050F + blueNoise * 0.008F;
        image.pixels[offset + 3] = 1.0F;
    }
    for (std::size_t index = 0; index < 2400; ++index) {
        const std::uint32_t x = 2 + nextRandom() % (image.width - 4);
        const std::uint32_t y = 2 + nextRandom() % (image.height - 4);
        const float value = 0.18F + static_cast<float>(nextRandom() % 720U) / 1000.0F;
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value * (0.90F + static_cast<float>(nextRandom() % 80U) / 1000.0F);
        image.pixels[offset + 2] = value * (0.88F + static_cast<float>(nextRandom() % 100U) / 1000.0F);
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(result.trails.empty());
}

void testArtifactTrailRejectsCrowdedDarkForegroundTexture() {
    photonstack::ImageBuffer image;
    image.width = 768;
    image.height = 512;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.026F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const bool foreground = y >= 360;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const float value = foreground ? 0.014F : 0.026F;
            image.pixels[offset] = value;
            image.pixels[offset + 1] = value;
            image.pixels[offset + 2] = value;
            image.pixels[offset + 3] = 1.0F;
        }
    }

    const auto paintPoint = [&](std::int32_t x, std::int32_t y, float value) {
        if (x < 1 || y < 1 || x >= static_cast<std::int32_t>(image.width - 1) ||
            y >= static_cast<std::int32_t>(image.height - 1)) {
            return;
        }
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const auto offset =
                    (static_cast<std::size_t>(y + dy) * image.width + static_cast<std::uint32_t>(x + dx)) *
                    image.channels;
                image.pixels[offset] = value;
                image.pixels[offset + 1] = value * 0.72F;
                image.pixels[offset + 2] = value * 0.55F;
            }
        }
    };
    for (std::size_t row = 0; row < 10; ++row) {
        const std::int32_t startX = 24 + static_cast<std::int32_t>((row * 61) % 190);
        const std::int32_t y = 374 + static_cast<std::int32_t>(row * 12);
        for (std::int32_t x = startX; x <= startX + 250; x += 18) {
            const std::int32_t wobble = ((x / 18 + static_cast<std::int32_t>(row)) % 3) - 1;
            paintPoint(x, y + wobble, 0.070F + static_cast<float>(row % 3) * 0.010F);
        }
    }

    const auto result = photonstack::ArtifactTrailRemover().detect(image);
    assert(result.ok);
    assert(result.trails.empty());
}

void testMeteorLayerExtractionAndRestore() {
    photonstack::ImageBuffer base;
    base.width = 64;
    base.height = 32;
    base.channels = 4;
    base.pixels.assign(base.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < base.pixelCount(); ++pixel) {
        base.pixels[pixel * base.channels + 3] = 1.0F;
    }

    photonstack::ImageBuffer source = base;
    const auto setPixel = [](photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value;
        target.pixels[offset + 2] = value;
    };
    for (std::uint32_t x = 8; x <= 56; ++x) {
        const float t = static_cast<float>(x - 8) / 48.0F;
        setPixel(source, x, 12, 0.9F - t * 0.65F);
    }

    const photonstack::MeteorLayerComposer composer;
    photonstack::MeteorLayerOptions options;
    options.detection.sigmaThreshold = 2.0F;
    options.detection.minPeak = 0.1F;
    options.detection.minLength = 10.0F;
    options.detection.airplaneLength = 30.0F;
    options.detection.maxWidth = 4.0F;
    options.minConfidence = 0.1F;
    options.maskRadius = 1.5F;
    options.featherRadius = 2.0F;

    const auto extracted = composer.extract(source, options);
    assert(extracted.ok);
    assert(extracted.meteors.size() == 1);
    assert(extracted.layer.pixels[(static_cast<std::size_t>(12) * source.width + 8) * source.channels + 3] > 0.1F);
    assert(extracted.layer.pixels[(static_cast<std::size_t>(2) * source.width + 2) * source.channels + 3] == 0.0F);

    const auto restored = composer.restore(base, source, options);
    assert(restored.ok);
    assert(restored.restoredMeteors == 1);
    const auto meteorHead = (static_cast<std::size_t>(12) * source.width + 8) * source.channels;
    const auto offTrail = (static_cast<std::size_t>(2) * source.width + 2) * source.channels;
    assert(restored.image.pixels[meteorHead] > base.pixels[meteorHead]);
    assert(restored.image.pixels[offTrail] == base.pixels[offTrail]);
}

void testMeteorLayerRespectsCoverageAndStraightAlpha() {
    photonstack::ImageBuffer source;
    source.width = 64;
    source.height = 32;
    source.channels = 4;
    source.pixels.assign(source.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < source.pixelCount(); ++pixel) {
        source.pixels[pixel * source.channels + 3] = 1.0F;
    }
    for (std::uint32_t x = 8; x <= 56; ++x) {
        const auto offset = (static_cast<std::size_t>(12) * source.width + x) * source.channels;
        const float t = static_cast<float>(x - 8) / 48.0F;
        source.pixels[offset] = 0.9F - t * 0.65F;
        source.pixels[offset + 1] = source.pixels[offset];
        source.pixels[offset + 2] = source.pixels[offset];
    }

    const auto hiddenOffset = (static_cast<std::size_t>(13) * source.width + 28) * source.channels;
    source.pixels[hiddenOffset] = 0.05F;
    source.pixels[hiddenOffset + 1] = 0.05F;
    source.pixels[hiddenOffset + 2] = 0.05F;
    source.pixels[hiddenOffset + 3] = 0.0F;
    const auto partialOffset = (static_cast<std::size_t>(13) * source.width + 30) * source.channels;
    source.pixels[partialOffset + 3] = 0.25F;

    photonstack::MeteorLayerOptions extractionOptions;
    extractionOptions.detection.sigmaThreshold = 2.0F;
    extractionOptions.detection.minPeak = 0.1F;
    extractionOptions.detection.minLength = 10.0F;
    extractionOptions.detection.airplaneLength = 30.0F;
    extractionOptions.detection.maxWidth = 4.0F;
    extractionOptions.minConfidence = 0.1F;
    extractionOptions.maskRadius = 1.5F;
    extractionOptions.featherRadius = 2.0F;

    const photonstack::MeteorLayerComposer composer;
    const auto extracted = composer.extract(source, extractionOptions);
    assert(extracted.ok);
    assert(!extracted.meteors.empty());
    assert(extracted.layer.pixels[hiddenOffset] == 0.0F);
    assert(extracted.layer.pixels[hiddenOffset + 1] == 0.0F);
    assert(extracted.layer.pixels[hiddenOffset + 2] == 0.0F);
    assert(extracted.layer.pixels[hiddenOffset + 3] == 0.0F);
    assert(extracted.layer.pixels[partialOffset + 3] > 0.0F);
    assert(extracted.layer.pixels[partialOffset + 3] <= 0.25F);

    photonstack::ImageBuffer base;
    base.width = 1;
    base.height = 1;
    base.channels = 4;
    base.pixels = {99.0F, 88.0F, 77.0F, 0.0F};
    photonstack::ImageBuffer layer = base;
    layer.pixels = {0.8F, 0.4F, 0.2F, 0.5F};
    photonstack::MeteorLayerOptions composeOptions;
    composeOptions.opacity = 0.5F;
    const auto transparentBase = composer.compose(base, layer, composeOptions);
    assert(transparentBase.ok);
    assert(std::fabs(transparentBase.image.pixels[0] - 0.8F) < 1.0e-6F);
    assert(std::fabs(transparentBase.image.pixels[1] - 0.4F) < 1.0e-6F);
    assert(std::fabs(transparentBase.image.pixels[2] - 0.2F) < 1.0e-6F);
    assert(std::fabs(transparentBase.image.pixels[3] - 0.25F) < 1.0e-6F);

    base.pixels = {2.0F, -1.0F, 0.5F, 1.0F};
    layer.pixels = {1.0F, 3.0F, -2.0F, 0.5F};
    const auto scientificRange = composer.compose(base, layer, composeOptions);
    assert(scientificRange.ok);
    assert(std::fabs(scientificRange.image.pixels[0] - 2.0F) < 1.0e-6F);
    assert(std::fabs(scientificRange.image.pixels[1]) < 1.0e-6F);
    assert(std::fabs(scientificRange.image.pixels[2] - 0.5F) < 1.0e-6F);
    assert(std::fabs(scientificRange.image.pixels[3] - 1.0F) < 1.0e-6F);

    layer.pixels = {100.0F, 100.0F, 100.0F, std::numeric_limits<float>::quiet_NaN()};
    const auto invalidLayerAlpha = composer.compose(base, layer, composeOptions);
    assert(invalidLayerAlpha.ok);
    assert(invalidLayerAlpha.image.pixels == base.pixels);
    assert(invalidLayerAlpha.restoredMeteors == 0);

    photonstack::ImageBuffer scientificSource = source;
    for (std::size_t pixel = 0; pixel < scientificSource.pixelCount(); ++pixel) {
        const auto offset = pixel * scientificSource.channels;
        if (scientificSource.pixels[offset + 3] <= 0.0F) {
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            scientificSource.pixels[offset + channel] = scientificSource.pixels[offset + channel] * 100.0F - 10.0F;
        }
    }
    scientificSource.sourceBitsPerChannel = 32;
    const auto scientificLayer = composer.extractUsingDetectionImage(scientificSource, source, extractionOptions);
    assert(scientificLayer.ok);
    assert(!scientificLayer.meteors.empty());
    const auto meteorOffset = (static_cast<std::size_t>(12) * source.width + 8) * source.channels;
    assert(scientificLayer.layer.pixels[meteorOffset] > 70.0F);
    assert(scientificLayer.layer.pixels[meteorOffset] == scientificSource.pixels[meteorOffset]);
    assert(scientificLayer.layer.sourceBitsPerChannel == scientificSource.sourceBitsPerChannel);

    photonstack::ImageBuffer scientificBase = scientificSource;
    for (std::size_t pixel = 0; pixel < scientificBase.pixelCount(); ++pixel) {
        const auto offset = pixel * scientificBase.channels;
        scientificBase.pixels[offset] = -5.0F;
        scientificBase.pixels[offset + 1] = -5.0F;
        scientificBase.pixels[offset + 2] = -5.0F;
        scientificBase.pixels[offset + 3] = 1.0F;
    }
    const auto scientificRestore = composer.restoreUsingDetectionImage(
        scientificBase,
        scientificSource,
        source,
        extractionOptions
    );
    assert(scientificRestore.ok);
    assert(scientificRestore.restoredMeteors == scientificRestore.meteors.size());
    assert(scientificRestore.image.pixels[meteorOffset] > scientificBase.pixels[meteorOffset]);
    assert(scientificRestore.image.pixels[meteorOffset] > 0.0F);
}

void testMeteorLayerRejectsLargeFrameWeakTaperStarStreaks() {
    photonstack::ImageBuffer source;
    source.width = 1024;
    source.height = 768;
    source.channels = 4;
    source.pixels.assign(source.sampleCount(), 0.045F);
    for (std::size_t pixel = 0; pixel < source.pixelCount(); ++pixel) {
        source.pixels[pixel * source.channels + 3] = 1.0F;
    }

    const auto setPixel = [](photonstack::ImageBuffer& target, std::int32_t x, std::int32_t y, float value) {
        if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(target.width) ||
            y >= static_cast<std::int32_t>(target.height)) {
            return;
        }
        const auto offset = (static_cast<std::size_t>(static_cast<std::uint32_t>(y)) * target.width +
                             static_cast<std::uint32_t>(x)) *
                            target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value;
        target.pixels[offset + 2] = value;
    };

    for (std::uint32_t i = 0; i <= 180; ++i) {
        const float t = static_cast<float>(i) / 180.0F;
        const auto x = static_cast<std::int32_t>(std::round(320.0F + t * 180.0F));
        const auto y = static_cast<std::int32_t>(std::round(430.0F + t * 42.0F));
        const float value = 0.74F - t * 0.18F;
        setPixel(source, x, y, value);
        if (i % 2 == 0) {
            setPixel(source, x, y + 1, value * 0.72F);
        }
    }

    photonstack::MeteorLayerOptions options;
    options.detection.sigmaThreshold = 1.8F;
    options.detection.minPeak = 0.06F;
    options.detection.minLength = 8.0F;
    options.detection.maxWidth = 8.0F;
    options.minConfidence = 0.1F;

    const photonstack::MeteorLayerComposer composer;
    const auto extracted = composer.extract(source, options);
    assert(extracted.ok);
    assert(extracted.meteors.empty());
    assert(extracted.restoredMeteors == 0);
}

void testArtifactTrailMergesOccludedAirplaneFragments() {
    photonstack::ImageBuffer image;
    image.width = 140;
    image.height = 60;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.035F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setPixel = [](photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value * 0.72F;
        target.pixels[offset + 2] = value * 0.55F;
    };

    for (std::uint32_t x = 14; x <= 38; ++x) {
        setPixel(image, x, 22, 0.78F);
    }
    for (std::uint32_t x = 64; x <= 88; ++x) {
        setPixel(image, x, 22, 0.75F);
    }
    for (std::uint32_t x = 112; x <= 132; ++x) {
        setPixel(image, x, 22, 0.8F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.0F, .minPeak = 0.08F, .minLength = 8.0F, .airplaneLength = 42.0F, .maxWidth = 4.0F});

    assert(result.ok);
    assert(!result.trails.empty());
    const auto airplane = std::find_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Airplane && trail.length > 100.0F;
    });
    assert(airplane != result.trails.end());
}

void testArtifactTrailDetectsBlinkingColoredAirplaneLights() {
    photonstack::ImageBuffer image;
    image.width = 180;
    image.height = 80;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.035F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
    };

    for (std::uint32_t x = 15; x <= 160; x += 18) {
        const std::uint32_t y = 55 + (x / 18) % 2;
        setRGB(image, x, y, 0.75F, 0.12F, 0.08F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.6F, 0.1F, 0.06F);
    }

    for (std::uint32_t x = 30; x <= 80; x += 10) {
        setRGB(image, x, 20, 0.32F, 0.34F, 0.36F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.2F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 45.0F, .maxWidth = 5.0F});

    assert(result.ok);
    const auto drone = std::find_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 120.0F;
    });
    assert(drone != result.trails.end());
}

void testArtifactTrailDetectsFaintRedBlinkingLights() {
    photonstack::ImageBuffer image;
    image.width = 180;
    image.height = 90;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.075F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint32_t x = 42 + i * 13;
        const std::uint32_t y = 58 + i / 3;
        for (std::uint32_t dy = 0; dy < 2; ++dy) {
            for (std::uint32_t dx = 0; dx < 2; ++dx) {
                const auto offset = (static_cast<std::size_t>(y + dy) * image.width + x + dx) * image.channels;
                image.pixels[offset] = 0.18F;
                image.pixels[offset + 1] = 0.075F;
                image.pixels[offset + 2] = 0.070F;
            }
        }
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F});

    assert(result.ok);
    assert(std::any_of(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 75.0F && trail.warmEvidence > 0.035F;
    }));
}

void testArtifactTrailDetectsFaintCyanBlinkingLights() {
    photonstack::ImageBuffer image;
    image.width = 260;
    image.height = 120;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.055F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
    };

    for (std::uint32_t i = 0; i < 12; ++i) {
        const std::uint32_t x = 34 + i * 16;
        const std::uint32_t y = 88 + i / 4;
        setRGB(image, x, y, 0.085F, 0.128F, 0.158F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.078F, 0.118F, 0.146F);
    }

    for (std::uint32_t x = 20; x < 240; x += 17) {
        setRGB(image, x, 32 + (x % 9), 0.18F, 0.18F, 0.19F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 42.0F, .maxWidth = 8.0F});

    assert(result.ok);
    assert(std::any_of(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 150.0F &&
               trail.colorVariance > 0.014F;
    }));
}

void testArtifactTrailDetectsTwoLowHorizonDottedDroneTrails() {
    photonstack::ImageBuffer image;
    image.width = 980;
    image.height = 420;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.070F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
    };

    for (std::uint32_t i = 0; i < 32; ++i) {
        const std::uint32_t x = 64 + i * 19;
        const std::uint32_t y = 318 + i;
        setRGB(image, x, y, 0.104F, 0.105F, 0.108F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.096F, 0.098F, 0.102F);
    }
    for (std::uint32_t i = 0; i < 17; ++i) {
        const std::uint32_t x = 430 + i * 31;
        const std::uint32_t y = 378 + i / 6;
        setRGB(image, x, y, 0.132F, 0.082F, 0.078F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.118F, 0.078F, 0.076F);
    }

    for (std::uint32_t i = 0; i < 80; ++i) {
        const std::uint32_t x = (i * 47 + 23) % image.width;
        const std::uint32_t y = (i * 31 + 17) % image.height;
        setRGB(image, x, y, 0.16F, 0.16F, 0.17F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F});

    assert(result.ok);
    const auto droneCount = std::count_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 220.0F && trail.y1 > 295.0F &&
               trail.y2 > 295.0F;
    });
    assert(droneCount >= 2);
}

void testArtifactTrailFollowsShortPeriodBottomHorizonDrone() {
    photonstack::ImageBuffer image;
    image.width = 2500;
    image.height = 900;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.070F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
    };
    const auto expectedY = [](float x) {
        const float index = (x - 1900.0F) / 19.0F;
        return 848.0F + index * 0.40F + index * index * 0.020F;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> droneDots;
    for (std::uint32_t index = 0; index < 24; ++index) {
        const std::uint32_t x = 1900 + index * 19;
        const std::uint32_t y = static_cast<std::uint32_t>(std::lround(expectedY(static_cast<float>(x))));
        droneDots.emplace_back(x, y);
        setRGB(image, x, y, 0.094F, 0.126F, 0.104F);
        setRGB(image, x + 1, y, 0.090F, 0.116F, 0.099F);
        setRGB(image, x, y - 1, 0.080F, 0.097F, 0.086F);
        setRGB(image, x, y + 1, 0.079F, 0.095F, 0.085F);
    }
    for (std::uint32_t index = 0; index < 120; ++index) {
        const std::uint32_t x = (index * 83 + 37) % image.width;
        const std::uint32_t y = (index * 47 + 19) % image.height;
        setRGB(image, x, y, 0.145F, 0.145F, 0.152F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F});

    assert(result.ok);
    const auto trail = std::find_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& item) {
        return item.kind == photonstack::ArtifactTrailKind::Drone && item.path.size() >= 4 &&
               item.length >= 330.0F && item.length <= 450.0F && item.x1 >= 1890.0F && item.x2 <= 2380.0F;
    });
    assert(trail != result.trails.end());
    float maximumPathError = 0.0F;
    for (const auto& point : trail->path) {
        maximumPathError = std::max(maximumPathError, std::fabs(point.y - expectedY(point.x)));
    }
    assert(maximumPathError < 8.0F);

    const auto removed = remover.remove(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F});
    assert(removed.ok);
    assert(removed.removedTrails >= 1);
    float maximumCenterError = 0.0F;
    float maximumCenterChroma = 0.0F;
    for (const auto& [x, y] : droneDots) {
        const auto center = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        const auto upper = (static_cast<std::size_t>(y - 12) * image.width + x) * image.channels;
        const auto lower = (static_cast<std::size_t>(y + 12) * image.width + x) * image.channels;
        float minimum = 1.0F;
        float maximum = 0.0F;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float expected = (removed.image.pixels[upper + channel] + removed.image.pixels[lower + channel]) * 0.5F;
            const float value = removed.image.pixels[center + channel];
            maximumCenterError = std::max(maximumCenterError, std::fabs(value - expected));
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        maximumCenterChroma = std::max(maximumCenterChroma, maximum - minimum);
    }
    assert(maximumCenterError < 0.010F);
    assert(maximumCenterChroma < 0.008F);
}

void testArtifactTrailDetectsAndRemovesOffsetLongPeriodBottomHorizonDrone() {
    photonstack::ImageBuffer image;
    image.width = 5000;
    image.height = 900;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            std::uint32_t hash = x * 1664525U + y * 1013904223U + 0x85ebca6bU;
            hash ^= hash >> 16U;
            const float grain = (static_cast<float>(hash & 1023U) / 1023.0F - 0.5F) * 0.006F;
            image.pixels[pixel * image.channels] = 0.270F + grain;
            image.pixels[pixel * image.channels + 1] = 0.300F + grain;
            image.pixels[pixel * image.channels + 2] = 0.385F + grain;
            image.pixels[pixel * image.channels + 3] = 1.0F;
        }
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
    };
    const auto luminance = [](const photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        return target.pixels[offset] * 0.2126F + target.pixels[offset + 1] * 0.7152F +
               target.pixels[offset + 2] * 0.0722F;
    };

    std::vector<std::pair<std::uint32_t, std::uint32_t>> trailDots;
    for (std::uint32_t index = 0; index < 18; ++index) {
        const std::uint32_t x = 4200 + index * 35;
        const std::uint32_t y = 884 - index / 3;
        trailDots.emplace_back(x, y);
        setRGB(image, x, y, 1.000F, 0.950F, 0.820F);
        setRGB(image, x + 1, y, 0.860F, 0.710F, 0.590F);
        setRGB(image, x, y + 1, 0.740F, 0.630F, 0.540F);
    }

    const photonstack::ArtifactTrailOptions options{
        .sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F};
    const photonstack::ArtifactTrailRemover remover;
    const auto detected = remover.detect(image, options);
    assert(detected.ok);
    const auto trail = std::find_if(detected.trails.begin(), detected.trails.end(), [](const auto& item) {
        return item.kind == photonstack::ArtifactTrailKind::Drone && item.path.size() >= 12 &&
               item.length > 540.0F && (item.y1 + item.y2) * 0.5F > 870.0F;
    });
    assert(trail != detected.trails.end());

    const auto removed = remover.remove(image, options);
    assert(removed.ok);
    float before = 0.0F;
    float after = 0.0F;
    float repairedSideDelta = 0.0F;
    for (const auto& [x, y] : trailDots) {
        before += luminance(image, x, y);
        after += luminance(removed.image, x, y);
        const float side = (luminance(removed.image, x, y - 8) + luminance(removed.image, x, y + 8)) * 0.5F;
        repairedSideDelta += std::fabs(luminance(removed.image, x, y) - side);
    }
    before /= static_cast<float>(trailDots.size());
    after /= static_cast<float>(trailDots.size());
    repairedSideDelta /= static_cast<float>(trailDots.size());
    assert(after < before - 0.12F);
    assert(repairedSideDelta < 0.020F);

    const auto redetected = remover.detect(removed.image, options);
    assert(redetected.ok);
    assert(std::none_of(redetected.trails.begin(), redetected.trails.end(), [](const auto& item) {
        return item.kind == photonstack::ArtifactTrailKind::Drone &&
               std::max(item.x1, item.x2) > 4100.0F && std::min(item.x1, item.x2) < 4900.0F &&
               (item.y1 + item.y2) * 0.5F > 850.0F;
    }));
}

void testArtifactTrailPrioritizesLowHorizonSparseDroneOverTexture() {
    photonstack::ImageBuffer image;
    image.width = 1200;
    image.height = 760;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.055F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
    };

    for (std::uint32_t x = 40; x < image.width - 40; x += 10) {
        const float texture = 0.066F + static_cast<float>((x / 10) % 4) * 0.002F;
        setRGB(image, x, 648 + ((x / 40) % 3), texture, texture * 0.98F, texture);
    }

    for (std::uint32_t i = 0; i < 13; ++i) {
        const std::uint32_t x = 360 + i * 42;
        const std::uint32_t y = 698 + i / 5;
        setRGB(image, x, y, 0.124F, 0.121F, 0.118F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.112F, 0.109F, 0.106F);
    }
    for (std::uint32_t i = 0; i < 18; ++i) {
        const std::uint32_t x = 820 + i * 20;
        const std::uint32_t y = 620 + i / 8;
        setRGB(image, x, y, 0.136F, 0.116F, 0.106F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.116F, 0.102F, 0.098F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto result = remover.detect(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F});

    assert(result.ok);
    assert(result.trails.size() == 2);
    const auto lowLeft = std::find_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 340.0F &&
               trail.x1 < 430.0F && trail.x2 > 720.0F && trail.y1 > 680.0F && trail.y2 > 680.0F;
    });
    const auto lowRight = std::find_if(result.trails.begin(), result.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 280.0F &&
               trail.x1 < 860.0F && trail.x2 > 1080.0F && trail.y1 > 600.0F && trail.y2 > 600.0F;
    });
    assert(lowLeft != result.trails.end());
    assert(lowRight != result.trails.end());

    const auto visibleEnd = result.trails.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(8, result.trails.size()));
    assert(std::find_if(result.trails.begin(), visibleEnd, [](const photonstack::ArtifactTrail& trail) {
               return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 340.0F &&
                      trail.x1 < 430.0F && trail.x2 > 720.0F && trail.y1 > 680.0F && trail.y2 > 680.0F;
           }) != visibleEnd);
    assert(std::find_if(result.trails.begin(), visibleEnd, [](const photonstack::ArtifactTrail& trail) {
               return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 280.0F &&
                      trail.x1 < 860.0F && trail.x2 > 1080.0F && trail.y1 > 600.0F && trail.y2 > 600.0F;
           }) != visibleEnd);
}

void testArtifactTrailRemovalCleansLowHorizonDottedDrone() {
    photonstack::ImageBuffer image;
    image.width = 1200;
    image.height = 760;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            std::uint32_t hash = x * 1664525U + y * 1013904223U + 0x9e3779b9U;
            hash ^= hash >> 16U;
            hash *= 2246822519U;
            hash ^= hash >> 13U;
            const float grain = (static_cast<float>(hash & 1023U) / 1023.0F - 0.5F) * 0.010F;
            image.pixels[pixel * image.channels] = 0.297F + grain * 0.85F;
            image.pixels[pixel * image.channels + 1] = 0.341F + grain;
            image.pixels[pixel * image.channels + 2] = 0.444F + grain * 1.05F;
            image.pixels[pixel * image.channels + 3] = 1.0F;
        }
    }

    const auto setRGB = [](photonstack::ImageBuffer& target,
                           std::uint32_t x,
                           std::uint32_t y,
                           float red,
                           float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
        target.pixels[offset + 3] = 1.0F;
    };
    const auto luminance = [](const photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        return target.pixels[offset] * 0.2126F + target.pixels[offset + 1] * 0.7152F +
               target.pixels[offset + 2] * 0.0722F;
    };

    for (std::uint32_t x = 40; x < image.width - 40; x += 10) {
        const float texture = 0.066F + static_cast<float>((x / 10) % 4) * 0.002F;
        setRGB(image, x, 648 + ((x / 40) % 3), texture, texture * 0.98F, texture);
    }

    for (std::uint32_t i = 0; i < 13; ++i) {
        const std::uint32_t x = 360 + i * 42;
        const std::uint32_t y = 698 + i / 5;
        setRGB(image, x, y, 0.124F, 0.121F, 0.118F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.112F, 0.109F, 0.106F);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> droneDots;
    for (std::uint32_t i = 0; i < 18; ++i) {
        const std::uint32_t x = 820 + i * 20;
        const std::uint32_t y = 620 + i / 8;
        droneDots.emplace_back(x, y);
        setRGB(image, x, y, 0.580F, 0.460F, 0.410F);
        setRGB(image, std::min<std::uint32_t>(x + 1, image.width - 1), y, 0.530F, 0.430F, 0.390F);
    }

    const photonstack::ArtifactTrailRemover remover;
    const auto removed = remover.remove(
        image,
        {.sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F});

    assert(removed.ok);
    assert(removed.removedTrails >= 1);
    const auto removedRight = std::find_if(removed.trails.begin(), removed.trails.end(), [](const photonstack::ArtifactTrail& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.length > 280.0F &&
               trail.x1 < 860.0F && trail.x2 > 1080.0F && trail.y1 > 600.0F && trail.y2 > 600.0F;
    });
    assert(removedRight != removed.trails.end());

    float beforeDotLuminance = 0.0F;
    float afterDotLuminance = 0.0F;
    for (const auto& [x, y] : droneDots) {
        beforeDotLuminance += luminance(image, x, y);
        afterDotLuminance += luminance(removed.image, x, y);
    }
    beforeDotLuminance /= static_cast<float>(droneDots.size());
    afterDotLuminance /= static_cast<float>(droneDots.size());
    assert(afterDotLuminance < beforeDotLuminance - 0.018F);

    float centerDelta = 0.0F;
    for (const auto& [x, y] : droneDots) {
        const float center = luminance(removed.image, x, y);
        const float side = (luminance(removed.image, x, y - 14) + luminance(removed.image, x, y + 14)) * 0.5F;
        centerDelta += center - side;
    }
    centerDelta /= static_cast<float>(droneDots.size());
    assert(centerDelta < 0.016F);
    assert(centerDelta > -0.012F);

    float centerBlueRed = 0.0F;
    float sideBlueRed = 0.0F;
    for (const auto& [x, y] : droneDots) {
        const auto center = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        const auto upper = (static_cast<std::size_t>(y - 14) * image.width + x) * image.channels;
        const auto lower = (static_cast<std::size_t>(y + 14) * image.width + x) * image.channels;
        centerBlueRed += removed.image.pixels[center + 2] - removed.image.pixels[center];
        sideBlueRed += ((removed.image.pixels[upper + 2] - removed.image.pixels[upper]) +
                        (removed.image.pixels[lower + 2] - removed.image.pixels[lower])) *
                       0.5F;
    }
    centerBlueRed /= static_cast<float>(droneDots.size());
    sideBlueRed /= static_cast<float>(droneDots.size());
    assert(std::fabs(centerBlueRed - sideBlueRed) < 0.003F);

    float maximumChannelError = 0.0F;
    for (const auto& [x, y] : droneDots) {
        const auto center = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        const auto upper = (static_cast<std::size_t>(y - 14) * image.width + x) * image.channels;
        const auto lower = (static_cast<std::size_t>(y + 14) * image.width + x) * image.channels;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float expected = (removed.image.pixels[upper + channel] + removed.image.pixels[lower + channel]) * 0.5F;
            maximumChannelError =
                std::max(maximumChannelError, std::fabs(removed.image.pixels[center + channel] - expected));
        }
    }
    assert(maximumChannelError < 0.035F);
}

void testArtifactTrailFollowsAndRemovesCurvedHorizonDrone() {
    photonstack::ImageBuffer image;
    image.width = 5000;
    image.height = 760;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * image.width + x;
            std::uint32_t hash = x * 1664525U + y * 1013904223U + 0x85ebca6bU;
            hash ^= hash >> 16U;
            hash *= 2246822519U;
            hash ^= hash >> 13U;
            const float grain = (static_cast<float>(hash & 1023U) / 1023.0F - 0.5F) * 0.001F;
            image.pixels[pixel * image.channels] = 0.052F + grain;
            image.pixels[pixel * image.channels + 1] = 0.052F + grain;
            image.pixels[pixel * image.channels + 2] = 0.052F + grain;
            image.pixels[pixel * image.channels + 3] = 1.0F;
        }
    }

    const auto setRGB = [](photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y, float red, float green,
                           float blue) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = red;
        target.pixels[offset + 1] = green;
        target.pixels[offset + 2] = blue;
        target.pixels[offset + 3] = 1.0F;
    };
    const auto luminance = [](const photonstack::ImageBuffer& target, std::uint32_t x, std::uint32_t y) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        return target.pixels[offset] * 0.2126F + target.pixels[offset + 1] * 0.7152F +
               target.pixels[offset + 2] * 0.0722F;
    };
    const auto expectedY = [](float x) {
        const float t = std::clamp((x - 1750.0F) / 680.0F, 0.0F, 1.0F);
        return 716.0F + 8.0F * t + 7.0F * 4.0F * t * (1.0F - t);
    };

    for (std::uint32_t x = 1500; x < 2700; x += 8) {
        const float texture = 0.060F + static_cast<float>((x / 8) % 4) * 0.0012F;
        setRGB(image, x, 687 + ((x / 32) % 2), texture, texture, texture);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> droneDots;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> faintCoolDots;
    for (std::uint32_t index = 0; index <= 17; ++index) {
        const std::uint32_t x = 1750 + index * 40;
        const std::uint32_t y = static_cast<std::uint32_t>(std::lround(expectedY(static_cast<float>(x))));
        droneDots.emplace_back(x, y);
        if (index % 6 == 1) {
            faintCoolDots.emplace_back(x, y);
            setRGB(image, x, y, 0.040F, 0.047F, 0.064F);
            setRGB(image, x + 1, y, 0.042F, 0.048F, 0.061F);
        } else {
            setRGB(image, x, y, 0.155F, 0.090F, 0.078F);
            setRGB(image, x + 1, y, 0.135F, 0.082F, 0.074F);
        }
    }
    for (std::uint32_t index = 0; index < 90; ++index) {
        const std::uint32_t x = (index * 83 + 29) % image.width;
        const std::uint32_t y = (index * 47 + 23) % 670;
        setRGB(image, x, y, 0.145F, 0.145F, 0.152F);
    }

    const photonstack::ArtifactTrailOptions options{
        .sigmaThreshold = 2.5F, .minPeak = 0.08F, .minLength = 10.0F, .airplaneLength = 40.0F, .maxWidth = 8.0F};
    const photonstack::ArtifactTrailRemover remover;
    const auto detected = remover.detect(image, options);
    assert(detected.ok);
    const auto curved = std::find_if(detected.trails.begin(), detected.trails.end(), [](const auto& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone && trail.path.size() >= 4 && trail.length > 480.0F &&
               std::min(trail.x1, trail.x2) < 1950.0F && std::max(trail.x1, trail.x2) > 2380.0F &&
               std::min(trail.y1, trail.y2) > 700.0F;
    });
    assert(curved != detected.trails.end());

    float pathError = 0.0F;
    for (const auto& point : curved->path) {
        pathError += std::fabs(point.y - expectedY(point.x));
    }
    pathError /= static_cast<float>(curved->path.size());
    assert(pathError < 6.0F);

    const auto removed = remover.remove(image, options);
    assert(removed.ok);
    float beforeLuminance = 0.0F;
    float afterLuminance = 0.0F;
    std::size_t coveredDots = 0;
    for (const auto& [x, y] : droneDots) {
        if (static_cast<float>(x) < std::min(curved->x1, curved->x2) ||
            static_cast<float>(x) > std::max(curved->x1, curved->x2)) {
            continue;
        }
        beforeLuminance += luminance(image, x, y);
        afterLuminance += luminance(removed.image, x, y);
        coveredDots += 1;
    }
    assert(coveredDots >= 10);
    beforeLuminance /= static_cast<float>(coveredDots);
    afterLuminance /= static_cast<float>(coveredDots);
    assert(afterLuminance < beforeLuminance - 0.020F);
    assert(afterLuminance < 0.0530F);

    float maximumFaintColorError = 0.0F;
    for (const auto& [x, y] : faintCoolDots) {
        const auto center = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        const auto upper = (static_cast<std::size_t>(y - 12) * image.width + x) * image.channels;
        const auto lower = (static_cast<std::size_t>(y + 12) * image.width + x) * image.channels;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float expected = (removed.image.pixels[upper + channel] + removed.image.pixels[lower + channel]) * 0.5F;
            maximumFaintColorError =
                std::max(maximumFaintColorError, std::fabs(removed.image.pixels[center + channel] - expected));
        }
    }
    assert(maximumFaintColorError < 0.006F);

    float centerNormalTexture = 0.0F;
    float sideNormalTexture = 0.0F;
    for (const auto& [x, y] : droneDots) {
        if (static_cast<float>(x) < std::min(curved->x1, curved->x2) ||
            static_cast<float>(x) > std::max(curved->x1, curved->x2)) {
            continue;
        }
        for (std::int32_t offset = -1; offset < 1; ++offset) {
            centerNormalTexture += std::fabs(
                luminance(removed.image, x, static_cast<std::uint32_t>(static_cast<std::int32_t>(y) + offset + 1)) -
                luminance(removed.image, x, static_cast<std::uint32_t>(static_cast<std::int32_t>(y) + offset)));
            sideNormalTexture += std::fabs(
                luminance(removed.image, x, static_cast<std::uint32_t>(static_cast<std::int32_t>(y) + 14 + offset + 1)) -
                luminance(removed.image, x, static_cast<std::uint32_t>(static_cast<std::int32_t>(y) + 14 + offset)));
        }
    }
    assert(centerNormalTexture > sideNormalTexture * 0.80F);
    assert(centerNormalTexture < sideNormalTexture * 1.80F);

    float maximumSideModification = 0.0F;
    for (const auto& [x, y] : droneDots) {
        for (const std::int32_t offset : {-20, -14, 14, 20}) {
            const auto sampleY = static_cast<std::uint32_t>(static_cast<std::int32_t>(y) + offset);
            const auto sample = (static_cast<std::size_t>(sampleY) * image.width + x) * image.channels;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float modification =
                    std::fabs(removed.image.pixels[sample + channel] - image.pixels[sample + channel]);
                maximumSideModification = std::max(maximumSideModification, modification);
            }
        }
    }
    assert(maximumSideModification < 2.0e-6F);

    const auto redetected = remover.detect(removed.image, options);
    assert(redetected.ok);
    assert(std::none_of(redetected.trails.begin(), redetected.trails.end(), [](const auto& trail) {
        return trail.kind == photonstack::ArtifactTrailKind::Drone &&
               std::min(trail.x1, trail.x2) < 1950.0F && std::max(trail.x1, trail.x2) > 2380.0F &&
               (trail.y1 + trail.y2) * 0.5F > 705.0F;
    }));
}

void testHistogram() {
    photonstack::ImageBuffer image;
    image.width = 4;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F, 1.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        std::numeric_limits<float>::quiet_NaN(), 0.5F, 0.5F, 0.0F,
    };

    const photonstack::Histogram histogram;
    const auto result = histogram.luminance(image, {.bins = 2});

    assert(result.ok);
    assert(result.bins.size() == 2);
    assert(result.bins[0] == 1);
    assert(result.bins[1] == 1);
    assert(std::fabs(result.mean - 0.5F) < 0.001F);

    photonstack::ImageBuffer weighted;
    weighted.width = 2;
    weighted.height = 1;
    weighted.channels = 4;
    weighted.pixels = {
        0.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F, 0.25F,
    };
    const auto weightedResult = histogram.luminance(weighted, {.bins = 2});
    assert(weightedResult.ok);
    assert(std::fabs(weightedResult.bins[0] - 1.0) < 1.0e-12);
    assert(std::fabs(weightedResult.bins[1] - 0.25) < 1.0e-12);
    assert(std::fabs(weightedResult.mean - 0.2F) < 1.0e-6F);

    auto visibleNonFinite = image;
    visibleNonFinite.pixels[15] = 1.0F;
    const auto invalidColor = histogram.luminance(visibleNonFinite);
    assert(!invalidColor.ok);
    assert(invalidColor.errorCode == "ImageBufferInvalid");

    auto nonFiniteAlpha = image;
    nonFiniteAlpha.pixels[15] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidAlpha = histogram.luminance(nonFiniteAlpha);
    assert(!invalidAlpha.ok);
    assert(invalidAlpha.errorCode == "ImageBufferInvalid");

    photonstack::ImageBuffer empty = image;
    for (std::size_t pixel = 0; pixel < empty.pixelCount(); ++pixel) {
        empty.pixels[pixel * empty.channels + 3] = 0.0F;
    }
    const auto emptyResult = histogram.luminance(empty);
    assert(!emptyResult.ok);
    assert(emptyResult.errorCode == "ImageCoverageEmpty");
}

void testStretch() {
    photonstack::ImageBuffer image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.pixels = {0.25F, 0.25F, 0.25F, 1.0F};

    const photonstack::Stretch stretch;
    const auto result = stretch.apply(image, {.blackPoint = 0.0F, .midPoint = 0.25F, .whitePoint = 1.0F});

    assert(result.ok);
    assert(std::fabs(result.image.pixels[0] - 0.5F) < 0.001F);
    assert(result.image.colorEncoding == photonstack::ColorEncoding::SRGB);
}

void testStretchPreservesColorRatio() {
    photonstack::ImageBuffer image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::Linear;
    image.pixels = {0.2F, 0.1F, 0.05F, 1.0F};

    const photonstack::Stretch stretch;
    const auto result = stretch.apply(image, {.blackPoint = 0.0F, .midPoint = 0.25F, .whitePoint = 1.0F});

    assert(result.ok);
    assert(std::fabs((result.image.pixels[0] / result.image.pixels[1]) - 2.0F) < 0.01F);
    assert(std::fabs((result.image.pixels[1] / result.image.pixels[2]) - 2.0F) < 0.01F);
}

void testParallelStretchMatchesSerialTiles() {
    photonstack::ImageBuffer input;
    input.width = 1024; input.height = 1027; input.channels = 4;
    input.pixels.resize(input.sampleCount());
    for (std::size_t p = 0; p < input.pixelCount(); ++p) {
        for (unsigned c = 0; c < 3; ++c)
            input.pixels[p * 4 + c] = static_cast<float>((p * 1237 + c * 17) % 197) / 71.0F - 0.5F;
        input.pixels[p * 4 + 3] = p % 17 == 0 ? 0 : p % 13 == 0 ? .25F : 1;
        if (p % 17 == 0) input.pixels[p * 4] = std::numeric_limits<float>::quiet_NaN();
    }
    for (bool preserve : {true, false}) {
        const photonstack::StretchOptions options{.blackPoint = -.3F, .midPoint = .17F, .whitePoint = 2,
                                                  .arcsinhStrength = preserve ? 0.0F : 8.0F, .preserveColor = preserve};
        const auto parallel = photonstack::Stretch().apply(input, options);
        assert(parallel.ok);
        for (unsigned y = 0; y < input.height; ++y) {
            photonstack::ImageBuffer row;
            row.width = input.width; row.height = 1; row.channels = 4;
            const auto first = input.pixels.begin() + y * input.width * 4;
            row.pixels.assign(first, first + input.width * 4);
            const auto serial = photonstack::Stretch().apply(row, options);
            assert(serial.ok);
            assert(std::equal(serial.image.pixels.begin(), serial.image.pixels.end(),
                              parallel.image.pixels.begin() + y * input.width * 4));
        }
    }
}

void testAutoStretchExactOrderStatistics() {
    // Compare with a deliberately sorted reference (including ties, signed
    // values, odd/even populations, and excluded pixels), not another nth_element.
    for (bool withMask : {false, true})
    for (unsigned count : {3U, 4U, 65U, 128U, 4097U}) {
        photonstack::ImageBuffer image;
        image.width = count + (withMask ? 1 : 0); image.height = 1; image.channels = 4;
        image.pixels.resize(image.sampleCount());
        std::vector<double> values;
        for (unsigned i = 0; i < count; ++i) {
            const float value = static_cast<float>((i * 7919U + 17U) % 251U) / 19.0F - 4.0F;
            for (unsigned c = 0; c < 3; ++c) image.pixels[i * 4 + c] = value;
            image.pixels[i * 4 + 3] = 1;
            values.push_back(static_cast<double>(value) * 0.2126 + static_cast<double>(value) * 0.7152 +
                             static_cast<double>(value) * 0.0722);
        }
        if (withMask) image.pixels[count * 4] = std::numeric_limits<float>::quiet_NaN(); // hidden, never included
        std::sort(values.begin(), values.end());
        const auto median = [](std::vector<double> samples) {
            std::sort(samples.begin(), samples.end());
            return samples.size() % 2 ? samples[samples.size() / 2]
                : (samples[samples.size() / 2 - 1] + samples[samples.size() / 2]) * 0.5;
        };
        const double background = median(values);
        auto deviations = values;
        for (auto& value : deviations) value = std::fabs(value - background);
        for (float clip : {0.75F, 0.999F, 1.0F}) {
            const photonstack::AutoStretchOptions options{.targetBackground = 0.18F, .highlightClip = clip};
            const float black = static_cast<float>(std::max(values.front(), background - options.shadowsSigma * median(deviations) * 1.4826));
            const float white = static_cast<float>(values[static_cast<std::size_t>(clip * static_cast<double>(count - 1))]);
            const double normalized = std::clamp((background - black) / (static_cast<double>(white) - black), 0.001, 0.999);
            const double gamma = std::log(static_cast<double>(options.targetBackground)) / std::log(normalized);
            const float mid = static_cast<float>(std::clamp(std::pow(0.5, 1.0 / gamma), 0.001, 0.999));
            const auto actual = photonstack::Stretch().estimateAuto(image, options);
            assert(actual.blackPoint == black);
            assert(actual.whitePoint == white);
            assert(actual.midPoint == mid);
        }
    }
}

void testAutoStretch() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.05F, 0.05F, 0.05F, 1.0F,
        0.1F, 0.1F, 0.1F, 1.0F,
        0.8F, 0.8F, 0.8F, 1.0F,
        0.0F, 100.0F, 0.0F, 0.0F,
        std::numeric_limits<float>::quiet_NaN(), 0.1F, 0.1F, 0.0F,
    };

    const photonstack::Stretch stretch;
    const auto result = stretch.applyAuto(image);

    assert(result.ok);
    assert(result.image.pixels[4] > image.pixels[4]);
    assert(result.image.pixels[12] == 0.0F);
    assert(result.image.pixels[13] == 0.0F);
    assert(result.image.pixels[14] == 0.0F);
    assert(result.image.pixels[15] == 0.0F);
    assert(result.image.pixels[16] == 0.0F);
    assert(result.image.pixels[19] == 0.0F);

    auto visibleNonFinite = image;
    visibleNonFinite.pixels[19] = 1.0F;
    const auto invalidVisible = stretch.applyAuto(visibleNonFinite);
    assert(!invalidVisible.ok);
    assert(invalidVisible.errorCode == "ImageBufferInvalid");

    photonstack::ImageBuffer display;
    display.width = 3;
    display.height = 1;
    display.channels = 4;
    display.pixels = {
        0.05F, 0.05F, 0.05F, 1.0F,
        0.10F, 0.10F, 0.10F, 1.0F,
        0.80F, 0.80F, 0.80F, 1.0F,
    };
    photonstack::ImageBuffer scientific = display;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            scientific.pixels[offset + channel] = -2.0F + 10.0F * display.pixels[offset + channel];
        }
    }
    const auto displayResult = stretch.applyAuto(display);
    const auto scientificResult = stretch.applyAuto(scientific);
    assert(displayResult.ok);
    assert(scientificResult.ok);
    for (std::size_t pixel = 0; pixel < display.pixelCount(); ++pixel) {
        const auto offset = pixel * display.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            assert(std::fabs(displayResult.image.pixels[offset + channel] -
                             scientificResult.image.pixels[offset + channel]) < 0.001F);
        }
    }

    photonstack::ImageBuffer constant;
    constant.width = 2;
    constant.height = 1;
    constant.channels = 4;
    constant.pixels = {1000.0F, 1000.0F, 1000.0F, 1.0F, 1000.0F, 1000.0F, 1000.0F, 1.0F};
    const auto constantResult = stretch.applyAuto(constant);
    assert(constantResult.ok);
    assert(std::isfinite(constantResult.image.pixels[0]));
    assert(std::fabs(constantResult.image.pixels[0] - 0.25F) < 0.01F);

    photonstack::ImageBuffer extreme;
    extreme.width = 3;
    extreme.height = 1;
    extreme.channels = 1;
    extreme.pixels = {
        -std::numeric_limits<float>::max(), 0.0F, std::numeric_limits<float>::max(),
    };
    const auto extremeResult = stretch.applyAuto(extreme);
    assert(extremeResult.ok);
    assert(std::isfinite(extremeResult.image.pixels[0]));
    assert(std::isfinite(extremeResult.image.pixels[1]));
    assert(std::isfinite(extremeResult.image.pixels[2]));
    assert(extremeResult.image.pixels[0] <= extremeResult.image.pixels[1]);
    assert(extremeResult.image.pixels[1] <= extremeResult.image.pixels[2]);
    assert(extremeResult.image.pixels[2] > 0.99F);

    photonstack::ImageBuffer cleanEstimate;
    cleanEstimate.width = 103;
    cleanEstimate.height = 1;
    cleanEstimate.channels = 4;
    cleanEstimate.pixels.assign(cleanEstimate.sampleCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < 100; ++pixel) {
        const auto offset = pixel * cleanEstimate.channels;
        const float value = pixel < 90 ? 0.10F : 0.80F;
        cleanEstimate.pixels[offset] = value;
        cleanEstimate.pixels[offset + 1] = value;
        cleanEstimate.pixels[offset + 2] = value;
        cleanEstimate.pixels[offset + 3] = 1.0F;
    }
    cleanEstimate.pixels[102 * cleanEstimate.channels] = 1.0F;
    cleanEstimate.pixels[102 * cleanEstimate.channels + 1] = 1.0F;
    cleanEstimate.pixels[102 * cleanEstimate.channels + 2] = 1.0F;
    auto partialEstimate = cleanEstimate;
    partialEstimate.pixels[100 * partialEstimate.channels + 3] = 0.01F;
    partialEstimate.pixels[101 * partialEstimate.channels + 3] = 0.01F;
    partialEstimate.pixels[102 * partialEstimate.channels + 3] = 0.01F;
    const auto cleanOptions = stretch.estimateAuto(cleanEstimate);
    const auto partialOptions = stretch.estimateAuto(partialEstimate);
    assert(std::fabs(cleanOptions.blackPoint - partialOptions.blackPoint) < 1.0e-6F);
    assert(std::fabs(cleanOptions.whitePoint - partialOptions.whitePoint) < 1.0e-6F);
    assert(std::fabs(cleanOptions.midPoint - partialOptions.midPoint) < 1.0e-6F);

    photonstack::ImageBuffer empty = display;
    for (std::size_t pixel = 0; pixel < empty.pixelCount(); ++pixel) {
        empty.pixels[pixel * empty.channels + 3] = 0.0F;
    }
    const auto emptyResult = stretch.applyAuto(empty);
    assert(!emptyResult.ok);
    assert(emptyResult.errorCode == "ImageCoverageEmpty");
}

void testCurves() {
    photonstack::ImageBuffer image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.pixels = {0.5F, 0.5F, 0.5F, 1.0F};

    const photonstack::Curves curves;
    photonstack::ImageBuffer linearImage;
    linearImage.width = 3;
    linearImage.height = 1;
    linearImage.channels = 4;
    linearImage.pixels = {
        0.25F, 0.25F, 0.25F, 1.0F, 0.5F, 0.5F, 0.5F, 1.0F, 0.75F, 0.75F, 0.75F, 1.0F,
    };
    const auto linearResult = curves.apply(linearImage, {.points = {{.input = 0.0F, .output = 0.0F},
                                                                    {.input = 0.5F, .output = 0.5F},
                                                                    {.input = 1.0F, .output = 1.0F}}});
    assert(linearResult.ok);
    for (std::size_t i = 0; i < linearImage.pixels.size(); ++i) {
        assert(std::fabs(linearResult.image.pixels[i] - linearImage.pixels[i]) < 0.001F);
    }

    const auto result = curves.apply(image, {.points = {{.input = 0.0F, .output = 0.0F},
                                                        {.input = 0.5F, .output = 0.75F},
                                                        {.input = 1.0F, .output = 1.0F}}});

    assert(result.ok);
    assert(result.image.pixels[0] > image.pixels[0]);
    assert(std::fabs(result.image.pixels[3] - 1.0F) < 0.001F);

    photonstack::ImageBuffer liftedShadow;
    liftedShadow.width = 1;
    liftedShadow.height = 1;
    liftedShadow.channels = 4;
    liftedShadow.pixels = {0.02F, 0.02F, 0.02F, 1.0F};
    const auto liftedResult = curves.apply(
        liftedShadow,
        {.points = {{.input = 0.0F, .output = 0.1F}, {.input = 1.0F, .output = 1.0F}}}
    );
    assert(liftedResult.ok);
    assert(liftedResult.image.pixels[0] > 0.10F);

    const auto redOnly = curves.apply(image, {.points = {{.input = 0.0F, .output = 0.0F},
                                                         {.input = 0.5F, .output = 0.75F},
                                                         {.input = 1.0F, .output = 1.0F}},
                                      .channel = photonstack::CurveChannel::Red});
    assert(redOnly.ok);
    assert(redOnly.image.pixels[0] > image.pixels[0]);
    assert(std::fabs(redOnly.image.pixels[1] - image.pixels[1]) < 0.001F);
    assert(std::fabs(redOnly.image.pixels[2] - image.pixels[2]) < 0.001F);

    photonstack::ImageBuffer deepSky;
    deepSky.width = 2;
    deepSky.height = 1;
    deepSky.channels = 4;
    deepSky.pixels = {
        0.018F, 0.016F, 0.014F, 1.0F,
        0.42F, 0.36F, 0.30F, 1.0F,
    };
    const std::vector<photonstack::CurvePoint> deepSkyPoints = {
        {.input = 0.0F, .output = 0.0F},
        {.input = 0.08F, .output = 0.08F},
        {.input = 0.5F, .output = 0.65F},
        {.input = 1.0F, .output = 1.0F},
    };
    const auto rgbResult = curves.apply(deepSky, {.points = deepSkyPoints});
    assert(rgbResult.ok);
    assert(rgbResult.image.pixels[0] > 0.014F);
    assert(rgbResult.image.pixels[1] > 0.012F);
    assert(rgbResult.image.pixels[4] > deepSky.pixels[4]);
    const float inputRatio = deepSky.pixels[4] / deepSky.pixels[5];
    const float rgbOutputRatio = rgbResult.image.pixels[4] / rgbResult.image.pixels[5];
    assert(std::fabs(inputRatio - rgbOutputRatio) > 0.005F);

    const auto luminanceResult = curves.apply(
        deepSky,
        {.points = deepSkyPoints, .channel = photonstack::CurveChannel::Luminance}
    );
    assert(luminanceResult.ok);
    const float luminanceOutputRatio = luminanceResult.image.pixels[4] / luminanceResult.image.pixels[5];
    assert(std::fabs(inputRatio - luminanceOutputRatio) < 0.001F);

    photonstack::ImageBuffer covered;
    covered.width = 3;
    covered.height = 1;
    covered.channels = 4;
    covered.pixels = {
        0.5F, 0.4F, 0.3F, 0.0F,
        std::numeric_limits<float>::quiet_NaN(), 0.4F, 0.3F, 0.0F,
        0.5F, 0.4F, 0.3F, 0.5F,
    };
    const auto coveredResult = curves.apply(
        covered,
        {.points = {{.input = 0.0F, .output = 0.0F},
                    {.input = 0.5F, .output = 0.75F},
                    {.input = 1.0F, .output = 1.0F}}}
    );
    assert(coveredResult.ok);
    for (std::size_t offset : {std::size_t{0}, std::size_t{4}}) {
        assert(coveredResult.image.pixels[offset] == 0.0F);
        assert(coveredResult.image.pixels[offset + 1] == 0.0F);
        assert(coveredResult.image.pixels[offset + 2] == 0.0F);
        assert(coveredResult.image.pixels[offset + 3] == 0.0F);
    }
    assert(coveredResult.image.pixels[8] > covered.pixels[8]);
    assert(coveredResult.image.pixels[11] == covered.pixels[11]);

    auto invalidVisible = covered;
    invalidVisible.pixels[7] = 1.0F;
    const auto invalidVisibleResult = curves.apply(
        invalidVisible,
        {.points = {{.input = 0.0F, .output = 0.0F}, {.input = 1.0F, .output = 1.0F}}}
    );
    assert(!invalidVisibleResult.ok);
    assert(invalidVisibleResult.errorCode == "ImageBufferInvalid");
}

void testLocalContrast() {
    photonstack::ImageBuffer image;
    image.width = 3;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.2F, 0.2F, 0.2F, 1.0F, 0.5F, 0.5F, 0.5F, 1.0F, 0.2F, 0.2F, 0.2F, 1.0F,
    };

    const photonstack::LocalContrast contrast;
    const auto result = contrast.apply(image, {.amount = 0.5F, .radius = 1});

    assert(result.ok);
    assert(result.image.pixels[4] > image.pixels[4]);
}

void testNoiseReduction() {
    photonstack::ImageBuffer image;
    image.width = 3;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.2F, 0.2F, 0.2F, 1.0F, 0.4F, 0.4F, 0.4F, 1.0F, 0.2F, 0.2F, 0.2F, 1.0F,
    };

    const photonstack::NoiseReducer reducer;
    const auto result = reducer.reduce(image, {.amount = 1.0F, .radius = 1, .edgeThreshold = 1.0F});

    assert(result.ok);
    assert(result.image.pixels[4] < image.pixels[4]);
}

void testSharpen() {
    photonstack::ImageBuffer image;
    image.width = 3;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.2F, 0.2F, 0.2F, 1.0F, 0.5F, 0.5F, 0.5F, 1.0F, 0.2F, 0.2F, 0.2F, 1.0F,
    };

    const photonstack::Sharpen sharpen;
    const auto result = sharpen.unsharpMask(image, {.amount = 1.0F, .radius = 1, .threshold = 0.0F});

    assert(result.ok);
    assert(result.image.pixels[4] > image.pixels[4]);
}

void testColorAdjuster() {
    photonstack::ImageBuffer image;
    image.width = 4;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.4F, 0.2F, 0.2F, 1.0F,
        0.6F, 0.3F, 0.3F, 1.0F,
        0.0F, 100.0F, 0.0F, 0.0F,
        std::numeric_limits<float>::quiet_NaN(), 0.4F, 0.2F, 0.0F,
    };

    const photonstack::ColorAdjuster adjuster;
    const auto neutralized = adjuster.neutralizeBackground(image);
    assert(neutralized.ok);
    assert(std::fabs(neutralized.redBackground - 0.5F) < 0.0001F);
    assert(std::fabs(neutralized.greenBackground - 0.25F) < 0.0001F);
    assert(std::fabs(neutralized.blueBackground - 0.25F) < 0.0001F);
    assert(neutralized.image.pixels[0] < image.pixels[0]);
    assert(neutralized.image.pixels[1] > image.pixels[1]);
    for (std::size_t offset : {std::size_t{8}, std::size_t{12}}) {
        assert(neutralized.image.pixels[offset] == 0.0F);
        assert(neutralized.image.pixels[offset + 1] == 0.0F);
        assert(neutralized.image.pixels[offset + 2] == 0.0F);
        assert(neutralized.image.pixels[offset + 3] == 0.0F);
    }

    const auto saturated = adjuster.adjustSaturation(neutralized.image, {.amount = 0.5F});
    assert(saturated.ok);
    assert(saturated.image.width == image.width);
    assert(saturated.image.height == image.height);
    assert(saturated.image.pixels[11] == 0.0F);
    assert(saturated.image.pixels[15] == 0.0F);

    photonstack::ImageBuffer greenCast;
    greenCast.width = 3;
    greenCast.height = 1;
    greenCast.channels = 4;
    greenCast.pixels = {
        0.10F, 0.24F, 0.10F, 1.0F,
        0.15F, 0.15F, 0.15F, 1.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
    };
    const auto suppressed = adjuster.suppressGreenCast(
        greenCast,
        {.amount = 1.0F, .backgroundLimit = 1.0F, .greenExcessThreshold = 0.01F}
    );
    assert(suppressed.ok);
    assert(suppressed.affectedPixels == 1);
    assert(suppressed.image.pixels[1] < greenCast.pixels[1]);
    assert(suppressed.image.pixels[0] == greenCast.pixels[0]);
    assert(suppressed.image.pixels[2] == greenCast.pixels[2]);
    assert(suppressed.image.pixels[8] == 0.0F);
    assert(suppressed.image.pixels[9] == 0.0F);
    assert(suppressed.image.pixels[10] == 0.0F);
    assert(suppressed.image.pixels[11] == 0.0F);

    photonstack::ImageBuffer partialStatistics;
    partialStatistics.width = 3;
    partialStatistics.height = 1;
    partialStatistics.channels = 4;
    partialStatistics.pixels = {
        0.4F, 0.2F, 0.2F, 1.0F,
        0.6F, 0.3F, 0.3F, 1.0F,
        1.0F, 0.0F, 1.0F, 0.01F,
    };
    const auto partialNeutralized = adjuster.neutralizeBackground(partialStatistics);
    assert(partialNeutralized.ok);
    assert(std::fabs(partialNeutralized.redBackground - 0.501F) < 0.002F);
    assert(std::fabs(partialNeutralized.greenBackground - 0.2495F) < 0.002F);
    assert(std::fabs(partialNeutralized.blueBackground - 0.2505F) < 0.002F);

    auto invalidVisible = image;
    invalidVisible.pixels[15] = 1.0F;
    const auto invalidNeutralized = adjuster.neutralizeBackground(invalidVisible);
    assert(!invalidNeutralized.ok);
    assert(invalidNeutralized.errorCode == "ImageBufferInvalid");
    const auto invalidSaturated = adjuster.adjustSaturation(invalidVisible);
    assert(!invalidSaturated.ok);
    assert(invalidSaturated.errorCode == "ImageBufferInvalid");
    const auto invalidSuppressed = adjuster.suppressGreenCast(invalidVisible);
    assert(!invalidSuppressed.ok);
    assert(invalidSuppressed.errorCode == "ImageBufferInvalid");

    auto emptyCoverage = partialStatistics;
    for (std::size_t pixel = 0; pixel < emptyCoverage.pixelCount(); ++pixel) {
        emptyCoverage.pixels[pixel * emptyCoverage.channels + 3] = 0.0F;
    }
    const auto emptyNeutralized = adjuster.neutralizeBackground(emptyCoverage);
    assert(!emptyNeutralized.ok);
    assert(emptyNeutralized.errorCode == "ImageCoverageEmpty");
    const auto emptySuppressed = adjuster.suppressGreenCast(emptyCoverage);
    assert(!emptySuppressed.ok);
    assert(emptySuppressed.errorCode == "ImageCoverageEmpty");

    photonstack::ImageBuffer display;
    display.width = 2;
    display.height = 1;
    display.channels = 4;
    display.pixels = {
        0.10F, 0.30F, 0.20F, 1.0F,
        0.60F, 0.80F, 0.70F, 1.0F,
    };
    const auto correctedDisplay = adjuster.neutralizeBackground(display, {.strength = 0.5F});
    assert(correctedDisplay.ok);
    photonstack::ImageBuffer scientific = display;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            scientific.pixels[offset + channel] = -2.0F + 10.0F * display.pixels[offset + channel];
        }
    }
    const auto bridged = photonstack::ScientificDisplayBridge().applyCorrections(
        scientific,
        display,
        correctedDisplay.image
    );
    assert(bridged.ok);
    std::size_t expectedCorrections = 0;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            const bool changed =
                std::fabs(correctedDisplay.image.pixels[offset + channel] - display.pixels[offset + channel]) >
                1.0e-7F;
            expectedCorrections += changed ? 1 : 0;
            const float expected = changed
                                       ? -2.0F + 10.0F * correctedDisplay.image.pixels[offset + channel]
                                       : scientific.pixels[offset + channel];
            assert(std::fabs(bridged.image.pixels[offset + channel] - expected) < 0.0001F);
        }
        assert(bridged.image.pixels[offset + 3] == scientific.pixels[offset + 3]);
    }
    assert(bridged.correctedSamples == expectedCorrections);
    assert(bridged.image.pixels[0] < 0.0F);
    assert(bridged.image.pixels[5] > 1.0F);
}

void testDeconvolution() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 5;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.05F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
    const auto center = (static_cast<std::size_t>(2) * image.width + 2) * image.channels;
    image.pixels[center] = 0.8F;
    image.pixels[center + 1] = 0.8F;
    image.pixels[center + 2] = 0.8F;

    const photonstack::Deconvolution deconvolution;
    const auto result = deconvolution.richardsonLucy(image, {.iterations = 2, .radius = 1, .sigma = 1.0F});

    assert(result.ok);
    assert(result.image.width == image.width);
    assert(result.image.height == image.height);
}

void testScientificNeighborhoodFiltersRespectCoverageAndRange() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 5;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 2.0F);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        image.pixels[pixel * image.channels + 3] = 1.0F;
    }
    const std::size_t maskedPixel = 2 * image.width + 2;
    const std::size_t maskedOffset = maskedPixel * image.channels;
    image.pixels[maskedOffset] = 0.0F;
    image.pixels[maskedOffset + 1] = 0.0F;
    image.pixels[maskedOffset + 2] = 0.0F;
    image.pixels[maskedOffset + 3] = 0.0F;

    const auto expectScientificUniform = [&](const photonstack::ImageBuffer& output) {
        for (std::size_t pixel = 0; pixel < output.pixelCount(); ++pixel) {
            const auto offset = pixel * output.channels;
            if (pixel == maskedPixel) {
                assert(output.pixels[offset] == 0.0F);
                assert(output.pixels[offset + 1] == 0.0F);
                assert(output.pixels[offset + 2] == 0.0F);
                assert(output.pixels[offset + 3] == 0.0F);
                continue;
            }
            assert(std::fabs(output.pixels[offset] - 2.0F) < 1.0e-4F);
            assert(std::fabs(output.pixels[offset + 1] - 2.0F) < 1.0e-4F);
            assert(std::fabs(output.pixels[offset + 2] - 2.0F) < 1.0e-4F);
            assert(output.pixels[offset + 3] == 1.0F);
        }
    };

    const auto denoised = photonstack::NoiseReducer().reduce(
        image,
        {.amount = 1.0F, .chromaAmount = 1.0F, .radius = 1, .edgeThreshold = 1.0F, .clampOutput = false}
    );
    assert(denoised.ok);
    expectScientificUniform(denoised.image);

    const auto contrasted = photonstack::LocalContrast().apply(
        image,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    assert(contrasted.ok);
    expectScientificUniform(contrasted.image);

    const auto sharpened = photonstack::Sharpen().unsharpMask(
        image,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    assert(sharpened.ok);
    expectScientificUniform(sharpened.image);

    const auto deconvolved = photonstack::Deconvolution().richardsonLucy(
        image,
        {.iterations = 1, .radius = 1, .sigma = 1.0F, .damping = 0.0F, .clampOutput = false}
    );
    assert(deconvolved.ok);
    expectScientificUniform(deconvolved.image);

    const auto displaySharpened = photonstack::Sharpen().unsharpMask(
        image,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F}
    );
    assert(displaySharpened.ok);
    assert(displaySharpened.image.pixels[0] == 1.0F);

    auto negativeImage = image;
    negativeImage.pixels[0] = -0.1F;
    const auto invalidDomain = photonstack::Deconvolution().richardsonLucy(
        negativeImage,
        {.iterations = 1, .radius = 1, .sigma = 1.0F, .damping = 0.0F, .clampOutput = false}
    );
    assert(!invalidDomain.ok);
    assert(invalidDomain.errorCode == "ImageValueDomainInvalid");

    photonstack::ImageBuffer transparentEdge;
    transparentEdge.width = 3;
    transparentEdge.height = 1;
    transparentEdge.channels = 4;
    transparentEdge.pixels = {
        0.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 1.0F, 1.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    auto partialEdge = transparentEdge;
    partialEdge.pixels[11] = 0.01F;

    const auto transparentDenoised = photonstack::NoiseReducer().reduce(
        transparentEdge,
        {.amount = 1.0F, .chromaAmount = 1.0F, .radius = 1, .edgeThreshold = 2.0F, .clampOutput = false}
    );
    const auto partialDenoised = photonstack::NoiseReducer().reduce(
        partialEdge,
        {.amount = 1.0F, .chromaAmount = 1.0F, .radius = 1, .edgeThreshold = 2.0F, .clampOutput = false}
    );
    assert(transparentDenoised.ok);
    assert(partialDenoised.ok);
    assert(std::fabs(transparentDenoised.image.pixels[4] - partialDenoised.image.pixels[4]) < 0.01F);

    const auto transparentContrasted = photonstack::LocalContrast().apply(
        transparentEdge,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    const auto partialContrasted = photonstack::LocalContrast().apply(
        partialEdge,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    assert(transparentContrasted.ok);
    assert(partialContrasted.ok);
    assert(std::fabs(transparentContrasted.image.pixels[4] - partialContrasted.image.pixels[4]) < 0.01F);

    const auto transparentSharpened = photonstack::Sharpen().unsharpMask(
        transparentEdge,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    const auto partialSharpened = photonstack::Sharpen().unsharpMask(
        partialEdge,
        {.amount = 1.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    assert(transparentSharpened.ok);
    assert(partialSharpened.ok);
    assert(std::fabs(transparentSharpened.image.pixels[4] - partialSharpened.image.pixels[4]) < 0.01F);

    auto visibleInvalid = transparentEdge;
    visibleInvalid.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    assert(photonstack::NoiseReducer().reduce(visibleInvalid).errorCode == "ImageBufferInvalid");
    assert(photonstack::LocalContrast().apply(visibleInvalid).errorCode == "ImageBufferInvalid");
    assert(photonstack::Sharpen().unsharpMask(visibleInvalid).errorCode == "ImageBufferInvalid");

    auto overflowing = transparentEdge;
    overflowing.pixels[0] = -std::numeric_limits<float>::max();
    overflowing.pixels[4] = std::numeric_limits<float>::max();
    overflowing.pixels[8] = -std::numeric_limits<float>::max();
    overflowing.pixels[11] = 1.0F;
    const auto overflowResult = photonstack::Sharpen().unsharpMask(
        overflowing,
        {.amount = 2.0F, .radius = 1, .threshold = 0.0F, .clampOutput = false}
    );
    assert(!overflowResult.ok);
    assert(overflowResult.errorCode == "ImageValueInvalid");

    const auto transparentDeconvolved = photonstack::Deconvolution().richardsonLucy(
        transparentEdge,
        {.iterations = 1, .radius = 1, .sigma = 1.0F, .damping = 0.0F, .clampOutput = false}
    );
    const auto partialDeconvolved = photonstack::Deconvolution().richardsonLucy(
        partialEdge,
        {.iterations = 1, .radius = 1, .sigma = 1.0F, .damping = 0.0F, .clampOutput = false}
    );
    assert(transparentDeconvolved.ok);
    assert(partialDeconvolved.ok);
    assert(std::fabs(transparentDeconvolved.image.pixels[4] - partialDeconvolved.image.pixels[4]) < 0.02F);

    photonstack::ImageBuffer scaleReference;
    scaleReference.width = 5;
    scaleReference.height = 5;
    scaleReference.channels = 1;
    scaleReference.pixels.assign(scaleReference.sampleCount(), 0.1F);
    scaleReference.pixels[2U * scaleReference.width + 2U] = 1.0F;
    auto tinyScale = scaleReference;
    constexpr float tinyFactor = 1.0e-10F;
    for (auto& sample : tinyScale.pixels) {
        sample *= tinyFactor;
    }
    const photonstack::DeconvolutionOptions scaleOptions = {
        .iterations = 2,
        .radius = 1,
        .sigma = 1.0F,
        .damping = 0.0F,
        .clampOutput = false,
    };
    const auto referenceScaleResult = photonstack::Deconvolution().richardsonLucy(scaleReference, scaleOptions);
    const auto tinyScaleResult = photonstack::Deconvolution().richardsonLucy(tinyScale, scaleOptions);
    assert(referenceScaleResult.ok);
    assert(tinyScaleResult.ok);
    for (std::size_t sample = 0; sample < scaleReference.pixels.size(); ++sample) {
        assert(std::fabs(tinyScaleResult.image.pixels[sample] / tinyFactor -
                         referenceScaleResult.image.pixels[sample]) < 1.0e-4F);
    }

    assert(photonstack::Deconvolution().richardsonLucy(visibleInvalid).errorCode == "ImageBufferInvalid");
}

void testScientificDisplayBridgeProjectsOnlyDisplayCorrections() {
    photonstack::ImageBuffer scientific;
    scientific.width = 4;
    scientific.height = 1;
    scientific.channels = 4;
    scientific.pixels = {
        -2.0F, -1.0F, 0.0F, 1.0F,
        2.0F, 4.0F, 6.0F, 1.0F,
        8.0F, 7.0F, 5.0F, 1.0F,
        0.0F, 0.0F, 0.0F, 0.0F,
    };
    photonstack::ImageBuffer display = scientific;
    for (std::size_t pixel = 0; pixel < 3; ++pixel) {
        const auto offset = pixel * display.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            display.pixels[offset + channel] = (scientific.pixels[offset + channel] + 2.0F) / 10.0F;
        }
    }
    display.pixels[12] = 0.2F;
    display.pixels[13] = 0.2F;
    display.pixels[14] = 0.2F;

    auto corrected = display;
    corrected.pixels[4] = 0.5F;
    corrected.pixels[12] = 0.9F;

    const photonstack::ScientificDisplayBridge bridge;
    const auto result = bridge.applyCorrections(scientific, display, corrected);
    assert(result.ok);
    assert(result.correctedSamples == 1);
    assert(std::fabs(result.displayToScientificScale - 10.0F) < 1.0e-5F);
    assert(std::fabs(result.displayToScientificOffset + 2.0F) < 1.0e-5F);
    assert(result.image.pixels[0] == scientific.pixels[0]);
    assert(std::fabs(result.image.pixels[4] - 3.0F) < 1.0e-5F);
    assert(result.image.pixels[5] == scientific.pixels[5]);
    assert(result.image.pixels[12] == scientific.pixels[12]);
    assert(result.image.pixels[15] == 0.0F);

    auto nonlinearDisplay = display;
    nonlinearDisplay.pixels[4] += 0.05F;
    const auto unsupported = bridge.applyCorrections(scientific, nonlinearDisplay, nonlinearDisplay);
    assert(!unsupported.ok);
    assert(unsupported.errorCode == "ImageValueMappingUnsupported");

    photonstack::ImageBuffer constant = scientific;
    for (std::size_t pixel = 0; pixel < constant.pixelCount(); ++pixel) {
        const auto offset = pixel * constant.channels;
        constant.pixels[offset] = 2.0F;
        constant.pixels[offset + 1] = 2.0F;
        constant.pixels[offset + 2] = 2.0F;
        constant.pixels[offset + 3] = 1.0F;
    }
    photonstack::ImageBuffer constantDisplay = constant;
    for (std::size_t pixel = 0; pixel < constantDisplay.pixelCount(); ++pixel) {
        const auto offset = pixel * constantDisplay.channels;
        constantDisplay.pixels[offset] = 1.0F;
        constantDisplay.pixels[offset + 1] = 1.0F;
        constantDisplay.pixels[offset + 2] = 1.0F;
    }
    const auto unchangedConstant = bridge.applyCorrections(constant, constantDisplay, constantDisplay);
    assert(unchangedConstant.ok);
    assert(unchangedConstant.correctedSamples == 0);
    auto changedConstant = constantDisplay;
    changedConstant.pixels[0] = 0.9F;
    const auto ambiguousConstant = bridge.applyCorrections(constant, constantDisplay, changedConstant);
    assert(!ambiguousConstant.ok);
    assert(ambiguousConstant.errorCode == "ImageValueMappingUnsupported");

    photonstack::ImageBuffer extreme;
    extreme.width = 2;
    extreme.height = 1;
    extreme.channels = 4;
    extreme.colorEncoding = photonstack::ColorEncoding::Linear;
    extreme.pixels = {
        -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(), 1.0F,
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(), 1.0F,
    };
    photonstack::ImageBuffer extremeDisplay = extreme;
    for (std::uint16_t channel = 0; channel < 3; ++channel) {
        extremeDisplay.pixels[channel] = 0.0F;
        extremeDisplay.pixels[4 + channel] = 1.0F;
    }
    auto extremeCorrected = extremeDisplay;
    extremeCorrected.pixels[0] = 0.25F;
    const auto extremeResult = bridge.applyCorrections(extreme, extremeDisplay, extremeCorrected);
    assert(extremeResult.ok);
    assert(std::isfinite(extremeResult.image.pixels[0]));
    assert(std::fabs(
        extremeResult.image.pixels[0] / std::numeric_limits<float>::max() + 0.5F) < 1.0e-6F);
    assert(extremeResult.displayToScientificScale >
           static_cast<double>(std::numeric_limits<float>::max()));

    extremeCorrected = extremeDisplay;
    extremeCorrected.pixels[0] = 2.0F;
    const auto extremeOverflow = bridge.applyCorrections(extreme, extremeDisplay, extremeCorrected);
    assert(!extremeOverflow.ok);
    assert(extremeOverflow.errorCode == "ImageValueInvalid");

    auto alphaMismatch = display;
    alphaMismatch.pixels[3] = 0.5F;
    const auto mismatchedCoverage = bridge.applyCorrections(scientific, display, alphaMismatch);
    assert(!mismatchedCoverage.ok);
    assert(mismatchedCoverage.errorCode == "ImageCoverageMismatch");

    auto encodingMismatch = display;
    encodingMismatch.colorEncoding = photonstack::ColorEncoding::SRGB;
    const auto mismatchedEncoding = bridge.applyCorrections(scientific, display, encodingMismatch);
    assert(!mismatchedEncoding.ok);
    assert(mismatchedEncoding.errorCode == "ImageColorEncodingMismatch");
}

void testScientificCloudRemovalBridgePreservesDynamicRange() {
    photonstack::ImageBuffer clear;
    clear.width = 160;
    clear.height = 120;
    clear.channels = 4;
    clear.pixels.resize(clear.sampleCount());
    for (std::uint32_t y = 0; y < clear.height; ++y) {
        const float gradient = 0.015F * static_cast<float>(y) / static_cast<float>(clear.height - 1);
        for (std::uint32_t x = 0; x < clear.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.035F + gradient;
            clear.pixels[offset + 1] = 0.050F + gradient;
            clear.pixels[offset + 2] = 0.080F + gradient;
            clear.pixels[offset + 3] = 1.0F;
        }
    }
    for (std::uint32_t y = 4; y < clear.height; y += 12) {
        for (std::uint32_t x = 4; x < clear.width; x += 12) {
            const auto offset = (static_cast<std::size_t>(y) * clear.width + x) * clear.channels;
            clear.pixels[offset] = 0.78F;
            clear.pixels[offset + 1] = 0.68F;
            clear.pixels[offset + 2] = 0.52F;
        }
    }

    photonstack::ImageBuffer veiled = clear;
    for (std::uint32_t y = 8; y < 112; ++y) {
        for (std::uint32_t x = 16; x < 144; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * veiled.width + x) * veiled.channels;
            veiled.pixels[offset] = std::min(1.0F, veiled.pixels[offset] + 0.14F);
            veiled.pixels[offset + 1] = std::min(1.0F, veiled.pixels[offset + 1] + 0.09F);
            veiled.pixels[offset + 2] = std::min(1.0F, veiled.pixels[offset + 2] + 0.05F);
        }
    }

    photonstack::CloudRemovalOptions options;
    options.columns = 20;
    options.rows = 15;
    options.minBrightnessDelta = 0.025F;
    options.minCoverage = 0.01F;
    options.strength = 1.0F;
    options.featherRadius = 0.0F;
    const auto removed = photonstack::CloudRemoval().remove(veiled, options);
    assert(removed.ok);
    assert(removed.removedRegions >= 1);

    photonstack::ImageBuffer scientific = veiled;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            scientific.pixels[offset + channel] = -2.0F + 10.0F * veiled.pixels[offset + channel];
        }
    }

    const auto bridged = photonstack::ScientificDisplayBridge().applyCorrections(scientific, veiled, removed.image);
    assert(bridged.ok);
    assert(bridged.correctedSamples > 0);
    assert(std::fabs(bridged.displayToScientificScale - 10.0F) < 1.0e-4F);
    assert(std::fabs(bridged.displayToScientificOffset + 2.0F) < 1.0e-4F);

    std::size_t expectedCorrections = 0;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            if (std::fabs(removed.image.pixels[offset + channel] - veiled.pixels[offset + channel]) <= 1.0e-7F) {
                assert(bridged.image.pixels[offset + channel] == scientific.pixels[offset + channel]);
            } else {
                const float expected = -2.0F + 10.0F * removed.image.pixels[offset + channel];
                assert(std::fabs(bridged.image.pixels[offset + channel] - expected) < 1.0e-4F);
                ++expectedCorrections;
            }
        }
        assert(bridged.image.pixels[offset + 3] == scientific.pixels[offset + 3]);
    }
    assert(bridged.correctedSamples == expectedCorrections);

    const auto darkOffset = (static_cast<std::size_t>(5) * scientific.width + 5) * scientific.channels;
    const auto brightOffset = (static_cast<std::size_t>(4) * scientific.width + 4) * scientific.channels;
    assert(bridged.image.pixels[darkOffset] < 0.0F);
    assert(bridged.image.pixels[brightOffset] > 1.0F);
    assert(bridged.image.pixels[darkOffset] == scientific.pixels[darkOffset]);
    assert(bridged.image.pixels[brightOffset] == scientific.pixels[brightOffset]);
}

void testScientificArtifactRemovalBridgePreservesDynamicRange() {
    photonstack::ImageBuffer display;
    display.width = 240;
    display.height = 120;
    display.channels = 4;
    display.pixels.assign(display.sampleCount(), 0.04F);
    for (std::size_t pixel = 0; pixel < display.pixelCount(); ++pixel) {
        display.pixels[pixel * display.channels + 3] = 1.0F;
    }

    const auto setNeutralPixel = [](photonstack::ImageBuffer& target,
                                    std::uint32_t x,
                                    std::uint32_t y,
                                    float value) {
        const auto offset = (static_cast<std::size_t>(y) * target.width + x) * target.channels;
        target.pixels[offset] = value;
        target.pixels[offset + 1] = value;
        target.pixels[offset + 2] = value;
    };
    for (std::uint32_t x = 28; x <= 212; ++x) {
        const auto y = static_cast<std::uint32_t>(50 + std::lround(static_cast<float>(x - 28) * 0.07F));
        setNeutralPixel(display, x, y, 0.52F);
        setNeutralPixel(display, x, y + 1, 0.31F);
    }
    for (std::uint32_t x = 18; x < 225; x += 29) {
        setNeutralPixel(display, x, 18 + (x % 23), 0.22F);
    }
    setNeutralPixel(display, 20, 100, 0.80F);

    photonstack::ArtifactTrailOptions options = {
        .sigmaThreshold = 2.0F,
        .minPeak = 0.08F,
        .minLength = 12.0F,
        .airplaneLength = 48.0F,
        .maxWidth = 5.0F,
    };
    options.removeAirplanes = false;
    options.removeDrones = false;
    options.removeSatellites = true;
    const auto removed = photonstack::ArtifactTrailRemover().remove(display, options);
    assert(removed.ok);
    assert(removed.removedTrails == 1);

    photonstack::ImageBuffer scientific = display;
    for (std::size_t pixel = 0; pixel < scientific.pixelCount(); ++pixel) {
        const auto offset = pixel * scientific.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            scientific.pixels[offset + channel] = -2.0F + 10.0F * display.pixels[offset + channel];
        }
    }

    const auto bridged = photonstack::ScientificDisplayBridge().applyCorrections(scientific, display, removed.image);
    assert(bridged.ok);
    assert(bridged.correctedSamples > 0);

    const auto correctedOffset = (static_cast<std::size_t>(56) * display.width + 120) * display.channels;
    assert(removed.image.pixels[correctedOffset] < display.pixels[correctedOffset] - 0.12F);
    assert(std::fabs(bridged.image.pixels[correctedOffset] -
                     (-2.0F + 10.0F * removed.image.pixels[correctedOffset])) < 1.0e-4F);

    const auto darkOffset = (static_cast<std::size_t>(5) * display.width + 5) * display.channels;
    const auto brightOffset = (static_cast<std::size_t>(100) * display.width + 20) * display.channels;
    assert(bridged.image.pixels[darkOffset] < 0.0F);
    assert(bridged.image.pixels[brightOffset] > 1.0F);
    for (std::uint16_t channel = 0; channel < 3; ++channel) {
        assert(bridged.image.pixels[darkOffset + channel] == scientific.pixels[darkOffset + channel]);
        assert(bridged.image.pixels[brightOffset + channel] == scientific.pixels[brightOffset + channel]);
    }
    assert(bridged.image.pixels[brightOffset + 3] == scientific.pixels[brightOffset + 3]);
}

void testDrizzleStacker() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 2;
    image.channels = 4;
    image.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
    };

    const auto path = std::filesystem::temp_directory_path() / "photonstack-drizzle.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, image, path).ok);

    const photonstack::DrizzleStacker drizzle;
    std::vector<photonstack::DrizzleProgress> progressEvents;
    photonstack::DrizzleOptions progressOptions = {
        .scale = 2,
        .pixfrac = 1.0F,
        .alignment = photonstack::DrizzleAlignment::None,
    };
    progressOptions.progress = [&](const photonstack::DrizzleProgress& progress) {
        progressEvents.push_back(progress);
    };
    const auto result = drizzle.drizzle({path}, progressOptions);

    assert(result.ok);
    assert(progressEvents.size() >= 5);
    assert(progressEvents.front().stage == photonstack::DrizzleProgressStage::Reading);
    assert(progressEvents.back().stage == photonstack::DrizzleProgressStage::Normalizing);
    assert(std::fabs(progressEvents.back().progress - 1.0) < 1.0e-9);
    assert(std::is_sorted(progressEvents.begin(), progressEvents.end(), [](const auto& left, const auto& right) {
        return left.progress < right.progress;
    }));
    assert(std::any_of(progressEvents.begin(), progressEvents.end(), [](const auto& progress) {
        return progress.stage == photonstack::DrizzleProgressStage::Accumulating;
    }));
    assert(result.image.width == 4);
    assert(result.image.height == 4);
    for (std::uint32_t y = 0; y < result.image.height; ++y) {
        for (std::uint32_t x = 0; x < result.image.width; ++x) {
            const auto sourceX = x / 2;
            const auto sourceY = y / 2;
            const auto sourceOffset = (static_cast<std::size_t>(sourceY) * image.width + sourceX) * image.channels;
            const auto outputOffset = (static_cast<std::size_t>(y) * result.image.width + x) * result.image.channels;
            for (std::uint16_t channel = 0; channel < image.channels; ++channel) {
                assert(std::fabs(result.image.pixels[outputOffset + channel] - image.pixels[sourceOffset + channel]) <
                       1.0e-6F);
            }
        }
    }

    image.width = 2;
    image.height = 1;
    image.pixels = {
        0.8F, 0.1F, 0.2F, 1.0F,
        0.0F, 0.9F, 0.1F, 0.0F,
    };
    const auto alphaPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-alpha.png";
    assert(writeSRGBFixture(codec, image, alphaPath).ok);
    const auto alphaResult = drizzle.drizzle(
        {alphaPath}, {.scale = 2, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(alphaResult.ok);
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * alphaResult.image.width + x) * 4;
            if (x < 2) {
                assert(alphaResult.image.pixels[offset + 3] > 0.999F);
                assert(alphaResult.image.pixels[offset] > 0.79F);
            } else {
                assert(alphaResult.image.pixels[offset + 3] == 0.0F);
                assert(alphaResult.image.pixels[offset] == 0.0F);
            }
        }
    }

    const auto invalidPixfrac = drizzle.drizzle(
        {path}, {.scale = 2, .pixfrac = 0.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(!invalidPixfrac.ok);
    assert(invalidPixfrac.errorCode == "ArgumentInvalid");

    photonstack::ImageBuffer gray;
    gray.width = 1;
    gray.height = 1;
    gray.channels = 1;
    gray.format = photonstack::PixelFormat::Float32Gray;
    gray.pixels = {0.4F};
    const auto grayPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-gray.fits";
    assert(codec.write(gray, grayPath).ok);
    const auto grayResult = drizzle.drizzle(
        {grayPath}, {.scale = 2, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(grayResult.ok);
    assert(grayResult.image.channels == 4);
    assert(grayResult.image.pixelCount() == 4);
    for (std::size_t pixel = 0; pixel < grayResult.image.pixelCount(); ++pixel) {
        const auto offset = pixel * grayResult.image.channels;
        assert(std::fabs(grayResult.image.pixels[offset] - 0.4F) < 1.0e-5F);
        assert(std::fabs(grayResult.image.pixels[offset + 1] - 0.4F) < 1.0e-5F);
        assert(std::fabs(grayResult.image.pixels[offset + 2] - 0.4F) < 1.0e-5F);
        assert(grayResult.image.pixels[offset + 3] > 0.999F);
    }

    photonstack::ImageBuffer second = image;
    second.width = 1;
    second.height = 1;
    second.pixels = {0.2F, 0.6F, 1.0F, 1.0F};
    const auto secondPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-second.png";
    assert(writeSRGBFixture(codec, second, secondPath).ok);
    image.width = 1;
    image.height = 1;
    image.pixels = {0.8F, 0.2F, 0.0F, 1.0F};
    const auto firstPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-first.png";
    assert(writeSRGBFixture(codec, image, firstPath).ok);
    const auto averaged = drizzle.drizzle(
        {firstPath, secondPath},
        {.scale = 2, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(averaged.ok);
    for (std::size_t pixel = 0; pixel < averaged.image.pixelCount(); ++pixel) {
        const auto offset = pixel * averaged.image.channels;
        assert(std::fabs(averaged.image.pixels[offset] - 0.5F) < 0.01F);
        assert(std::fabs(averaged.image.pixels[offset + 1] - 0.4F) < 0.01F);
        assert(std::fabs(averaged.image.pixels[offset + 2] - 0.5F) < 0.01F);
        assert(averaged.image.pixels[offset + 3] > 0.999F);
    }

    photonstack::ImageBuffer mismatched = image;
    mismatched.width = 2;
    mismatched.pixels = {
        0.1F, 0.1F, 0.1F, 1.0F,
        0.2F, 0.2F, 0.2F, 1.0F,
    };
    const auto mismatchedPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-mismatched.png";
    assert(writeSRGBFixture(codec, mismatched, mismatchedPath).ok);
    const auto mismatch = drizzle.drizzle(
        {firstPath, mismatchedPath},
        {.scale = 2, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(!mismatch.ok);
    assert(mismatch.errorCode == "ImageDimensionsMismatch");

    const auto missingPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-missing.png";
    std::error_code ignored;
    std::filesystem::remove(missingPath, ignored);
    const auto missing = drizzle.drizzle(
        {firstPath, missingPath},
        {.scale = 2, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(!missing.ok);
    assert(!missing.errorCode.empty());

    photonstack::ImageBuffer maximumImage;
    maximumImage.width = 1;
    maximumImage.height = 1;
    maximumImage.channels = 4;
    maximumImage.colorEncoding = photonstack::ColorEncoding::Linear;
    maximumImage.sourceBitsPerChannel = 32;
    const float maximum = std::numeric_limits<float>::max();
    maximumImage.pixels = {maximum, maximum, maximum, 1.0F};
    const auto maximumAPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-maximum-a.fits";
    const auto maximumBPath = std::filesystem::temp_directory_path() / "photonstack-drizzle-maximum-b.fits";
    assert(codec.write(maximumImage, maximumAPath).ok);
    assert(codec.write(maximumImage, maximumBPath).ok);
    const auto maximumResult = drizzle.drizzle(
        {maximumAPath, maximumBPath},
        {.scale = 1, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None}
    );
    assert(maximumResult.ok);
    assert(maximumResult.image.pixels[0] == maximum);
    assert(maximumResult.image.pixels[1] == maximum);
    assert(maximumResult.image.pixels[2] == maximum);
    assert(maximumResult.image.pixels[3] == 1.0F);

    photonstack::ImageBuffer partialBase = maximumImage;
    partialBase.pixels = {0.0F, 0.0F, 0.0F, 1.0F};
    photonstack::ImageBuffer partialEdge = maximumImage;
    partialEdge.pixels = {100.0F, 100.0F, 100.0F, 0.0001F};
    const auto partialBasePath = std::filesystem::temp_directory_path() / "photonstack-drizzle-partial-base.fits";
    const auto partialEdgePath = std::filesystem::temp_directory_path() / "photonstack-drizzle-partial-edge.fits";
    assert(codec.write(partialBase, partialBasePath).ok);
    assert(codec.write(partialEdge, partialEdgePath).ok);
    const auto partialResult = drizzle.drizzle(
        {partialBasePath, partialEdgePath},
        {.scale = 1, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None}
    );
    assert(partialResult.ok);
    const float expectedPartial = 100.0F * 0.0001F / 1.0001F;
    assert(std::fabs(partialResult.image.pixels[0] - expectedPartial) < 1.0e-6F);
    assert(std::fabs(partialResult.image.pixels[1] - expectedPartial) < 1.0e-6F);
    assert(std::fabs(partialResult.image.pixels[2] - expectedPartial) < 1.0e-6F);
    assert(partialResult.image.pixels[3] == 1.0F);
}

void testMultiImageOperationsRejectMixedColorEncodings() {
    photonstack::ImageBuffer srgb;
    srgb.width = 2;
    srgb.height = 2;
    srgb.channels = 4;
    srgb.colorEncoding = photonstack::ColorEncoding::SRGB;
    srgb.pixels.assign(srgb.sampleCount(), 0.5F);
    for (std::size_t pixel = 0; pixel < srgb.pixelCount(); ++pixel) {
        srgb.pixels[pixel * srgb.channels + 3] = 1.0F;
    }

    photonstack::ImageBuffer linear = srgb;
    linear.colorEncoding = photonstack::ColorEncoding::Linear;
    for (std::size_t pixel = 0; pixel < linear.pixelCount(); ++pixel) {
        const auto offset = pixel * linear.channels;
        linear.pixels[offset] = 0.214041F;
        linear.pixels[offset + 1] = 0.214041F;
        linear.pixels[offset + 2] = 0.214041F;
    }

    const auto srgbPath = std::filesystem::temp_directory_path() / "photonstack-mixed-encoding.png";
    const auto linearPath = std::filesystem::temp_directory_path() / "photonstack-mixed-encoding.fits";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, srgb, srgbPath).ok);
    assert(codec.write(linear, linearPath).ok);

    const auto cachesBefore = stackCacheDirectories();
    const photonstack::Stacker stacker;
    const auto average = stacker.average({srgbPath, linearPath});
    const auto median = stacker.median({srgbPath, linearPath});
    for (const auto* result : {&average, &median}) {
        assert(!result->ok);
        assert(result->errorCode == "ImageColorEncodingMismatch");
    }
    assert(stackCacheDirectories() == cachesBefore);

    const auto drizzle = photonstack::DrizzleStacker().drizzle(
        {srgbPath, linearPath},
        {.scale = 2, .pixfrac = 1.0F, .alignment = photonstack::DrizzleAlignment::None});
    assert(!drizzle.ok);
    assert(drizzle.errorCode == "ImageColorEncodingMismatch");

    const auto mosaic = photonstack::MosaicBuilder().stitchHorizontal(
        {srgb, linear},
        {.projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false});
    assert(!mosaic.ok);
    assert(mosaic.errorCode == "ImageColorEncodingMismatch");

    const auto calibrated = photonstack::Calibrator().calibrate(srgb, {.dark = &linear});
    assert(!calibrated.ok);
    assert(calibrated.errorCode == "ImageColorEncodingMismatch");

    const photonstack::MeteorLayerComposer meteorComposer;
    const auto composedMeteor = meteorComposer.compose(srgb, linear);
    assert(!composedMeteor.ok);
    assert(composedMeteor.errorCode == "ImageColorEncodingMismatch");
    const auto restoredMeteor = meteorComposer.restore(srgb, linear);
    assert(!restoredMeteor.ok);
    assert(restoredMeteor.errorCode == "ImageColorEncodingMismatch");

    const photonstack::FrameNormalizer normalizer;
    const auto normalized = normalizer.matchReference(srgb, linear);
    assert(!normalized.ok);
    assert(normalized.errorCode == "ImageColorEncodingMismatch");
    const auto localNormalized = normalizer.matchReferenceLocal(
        srgb, linear, {.normalization = {}, .columns = 1, .rows = 1});
    assert(!localNormalized.ok);
    assert(localNormalized.errorCode == "ImageColorEncodingMismatch");

    auto sameEncodingDifferentBitDepth = srgb;
    sameEncodingDifferentBitDepth.sourceBitsPerChannel = 16;
    const auto compatibleMosaic = photonstack::MosaicBuilder().stitchHorizontal(
        {srgb, sameEncodingDifferentBitDepth},
        {.overlapPixels = 1, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false});
    assert(compatibleMosaic.ok);
}

void testDrizzleDistortionAlignmentSharpensWideFieldEdges() {
    constexpr std::uint32_t width = 800;
    constexpr std::uint32_t height = 520;
    const float centerX = static_cast<float>(width) * 0.5F;
    const float centerY = static_cast<float>(height) * 0.5F;
    std::vector<std::pair<float, float>> stars;
    stars.reserve(90);
    std::uint32_t state = 0x243F6A88U;
    while (stars.size() < 90) {
        state = state * 1664525U + 1013904223U;
        const float x = 65.0F + static_cast<float>(state % 670U);
        state = state * 1664525U + 1013904223U;
        const float y = 55.0F + static_cast<float>(state % 410U);
        const bool separated = std::all_of(stars.begin(), stars.end(), [&](const auto& existing) {
            const float dx = existing.first - x;
            const float dy = existing.second - y;
            return dx * dx + dy * dy >= 15.0F * 15.0F;
        });
        if (separated) {
            stars.push_back({x, y});
        }
    }

    std::vector<std::filesystem::path> paths;
    const photonstack::ImageCodec codec;
    for (std::size_t frame = 0; frame < 4; ++frame) {
        photonstack::ImageBuffer image;
        image.width = width;
        image.height = height;
        image.channels = 4;
        image.pixels.assign(image.sampleCount(), 0.0F);
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            image.pixels[pixel * 4 + 3] = 1.0F;
        }

        const float angle = static_cast<float>(frame) * 0.8F * 3.14159265358979323846F / 180.0F;
        const float cosTheta = std::cos(angle);
        const float sinTheta = std::sin(angle);
        const float shiftX = static_cast<float>(frame) * 2.4F;
        const float shiftY = static_cast<float>(frame) * -1.8F;
        const float radialCoefficient = static_cast<float>(frame) * 5.0e-8F;
        float peak = 1.0F;
        for (const auto& [x, y] : stars) {
            const float localX = x - centerX;
            const float localY = y - centerY;
            const float rotatedX = cosTheta * localX - sinTheta * localY;
            const float rotatedY = sinTheta * localX + cosTheta * localY;
            const float radiusSquared = rotatedX * rotatedX + rotatedY * rotatedY;
            addGaussianStar(image,
                            centerX + rotatedX + rotatedX * radialCoefficient * radiusSquared + shiftX,
                            centerY + rotatedY + rotatedY * radialCoefficient * radiusSquared + shiftY,
                            peak);
            peak = peak <= 0.58F ? 1.0F : peak - 0.03F;
        }

        const auto path = std::filesystem::temp_directory_path() /
                          ("photonstack-drizzle-distorted-" + std::to_string(frame) + ".png");
        assert(writeSRGBFixture(codec, image, path).ok);
        paths.push_back(path);
    }

    const photonstack::RegistrationOptions registrationOptions = {
        .starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.05F, .maxStars = 500},
        .matchTolerance = 3.0F,
        .minimumMatches = 16,
    };
    const photonstack::DrizzleStacker drizzle;
    const auto translated = drizzle.drizzle(
        paths,
        {.scale = 1,
         .pixfrac = 1.0F,
         .alignment = photonstack::DrizzleAlignment::Translation,
         .registration = registrationOptions});
    std::vector<photonstack::DrizzleProgress> distortionProgress;
    photonstack::DrizzleOptions distortionOptions = {
        .scale = 1,
        .pixfrac = 1.0F,
        .alignment = photonstack::DrizzleAlignment::Distortion,
        .registration = registrationOptions,
    };
    distortionOptions.progress = [&](const photonstack::DrizzleProgress& progress) {
        distortionProgress.push_back(progress);
    };
    const auto corrected = drizzle.drizzle(paths, distortionOptions);

    assert(translated.ok);
    assert(corrected.ok);
    assert(corrected.alignedFrames == 3);
    assert(corrected.alignmentFallbacks == 0);
    assert(corrected.minimumAlignmentMatches >= 35);
    assert(std::any_of(distortionProgress.begin(), distortionProgress.end(), [](const auto& progress) {
        return progress.stage == photonstack::DrizzleProgressStage::Aligning;
    }));
    assert(std::is_sorted(distortionProgress.begin(), distortionProgress.end(), [](const auto& left, const auto& right) {
        return left.progress < right.progress;
    }));

    const photonstack::StarDetector detector;
    const photonstack::StarDetectionOptions detection = {
        .sigmaThreshold = 2.4F,
        .minPeak = 0.04F,
        .maxStars = 1000,
    };
    const auto translatedStars = detector.detect(translated.image, detection);
    const auto correctedStars = detector.detect(corrected.image, detection);
    assert(translatedStars.ok);
    assert(correctedStars.ok);
    assert(correctedStars.stars.size() >= 75);
    assert(correctedStars.stars.size() <= 125);
    assert(correctedStars.stars.size() * 5 < translatedStars.stars.size() * 4);

    const auto edgeSharpness = [&](const photonstack::ImageBuffer& image) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& [x, y] : stars) {
            const float dx = x - centerX;
            const float dy = y - centerY;
            if (dx * dx + dy * dy < 230.0F * 230.0F) {
                continue;
            }
            const auto px = static_cast<std::uint32_t>(std::lround(x));
            const auto py = static_cast<std::uint32_t>(std::lround(y));
            const auto offset = (static_cast<std::size_t>(py) * image.width + px) * image.channels;
            sum += image.pixels[offset];
            ++count;
        }
        assert(count >= 20);
        return sum / static_cast<double>(count);
    };
    assert(edgeSharpness(corrected.image) > edgeSharpness(translated.image) * 1.35);
}

void testMasterFrameBuilder() {
    photonstack::ImageBuffer dark;
    dark.width = 1;
    dark.height = 1;
    dark.channels = 4;
    dark.pixels = {0.1F, 0.1F, 0.1F, 1.0F};

    photonstack::ImageBuffer bright;
    bright.width = 1;
    bright.height = 1;
    bright.channels = 4;
    bright.pixels = {0.9F, 0.9F, 0.9F, 1.0F};

    const auto darkPath = std::filesystem::temp_directory_path() / "photonstack-master-dark.png";
    const auto brightPath = std::filesystem::temp_directory_path() / "photonstack-master-bright.png";

    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, dark, darkPath).ok);
    assert(writeSRGBFixture(codec, bright, brightPath).ok);

    const photonstack::MasterFrameBuilder builder;
    const auto result = builder.build({darkPath, brightPath}, {.method = photonstack::MasterFrameMethod::Average});

    assert(!result.ok);
    assert(result.errorCode == "ImageColorEncodingMismatch");

    photonstack::ImageBuffer scientificDark = dark;
    scientificDark.colorEncoding = photonstack::ColorEncoding::Linear;
    scientificDark.sourceBitsPerChannel = 32;
    scientificDark.pixels = {-2.0F, -2.0F, -2.0F, 1.0F};
    photonstack::ImageBuffer scientificBright = scientificDark;
    scientificBright.pixels = {4.0F, 4.0F, 4.0F, 1.0F};
    const auto scientificDarkPath = std::filesystem::temp_directory_path() / "photonstack-master-scientific-dark.fits";
    const auto scientificBrightPath = std::filesystem::temp_directory_path() / "photonstack-master-scientific-bright.fits";
    assert(codec.write(scientificDark, scientificDarkPath).ok);
    assert(codec.write(scientificBright, scientificBrightPath).ok);

    const auto scientificResult = builder.build(
        {scientificDarkPath, scientificBrightPath},
        {.method = photonstack::MasterFrameMethod::Average}
    );
    assert(scientificResult.ok);
    assert(std::fabs(scientificResult.image.pixels[0] - 1.0F) < 0.001F);
    assert(scientificResult.image.colorEncoding == photonstack::ColorEncoding::Linear);
    assert(scientificResult.image.sourceBitsPerChannel == 32);

    scientificBright.pixels = {100.0F, 100.0F, 100.0F, 0.0F};
    assert(codec.write(scientificBright, scientificBrightPath).ok);
    const auto partialMaster = builder.build(
        {scientificDarkPath, scientificBrightPath},
        {.method = photonstack::MasterFrameMethod::Average}
    );
    assert(partialMaster.ok);
    assert(std::fabs(partialMaster.image.pixels[0] + 2.0F) < 0.001F);
    assert(std::fabs(partialMaster.image.pixels[3] - 0.5F) < 0.001F);

    photonstack::ImageBuffer calibrationLight = scientificDark;
    calibrationLight.pixels = {3.0F, 3.0F, 3.0F, 1.0F};
    const auto partialCalibration = photonstack::Calibrator().calibrate(
        calibrationLight,
        {.dark = &partialMaster.image, .clampNegativeValues = false}
    );
    assert(partialCalibration.ok);
    assert(std::fabs(partialCalibration.image.pixels[0] - 5.0F) < 0.001F);
    assert(std::fabs(partialCalibration.image.pixels[3] - 0.5F) < 0.001F);

    const auto invalidMethod = builder.build(
        {scientificDarkPath},
        {.method = static_cast<photonstack::MasterFrameMethod>(999)}
    );
    assert(!invalidMethod.ok);
    assert(invalidMethod.errorCode == "MethodUnsupported");
}

void testMosaicBuilder() {
    photonstack::ImageBuffer left;
    left.width = 2;
    left.height = 1;
    left.channels = 4;
    left.pixels = {
        0.1F, 0.1F, 0.1F, 1.0F, 0.2F, 0.2F, 0.2F, 1.0F,
    };

    photonstack::ImageBuffer right;
    right.width = 2;
    right.height = 1;
    right.channels = 4;
    right.pixels = {
        0.8F, 0.8F, 0.8F, 1.0F, 0.9F, 0.9F, 0.9F, 1.0F,
    };

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal({left, right}, {.overlapPixels = 1});

    assert(result.ok);
    assert(result.image.width == 3);
    assert(result.image.height == 1);
    assert(std::fabs(result.image.pixels[0] - 0.1F) < 0.001F);
    assert(result.image.pixels[4] > 0.2F);
    assert(result.image.pixels[8] > 0.8F);
}

void testMosaicBuilderProgress() {
    photonstack::ImageBuffer left;
    left.width = 64;
    left.height = 32;
    left.channels = 4;
    left.pixels.assign(left.sampleCount(), 0.0F);
    for (std::uint32_t y = 0; y < left.height; ++y) {
        for (std::uint32_t x = 0; x < left.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * left.width + x) * left.channels;
            left.pixels[offset] = static_cast<float>(x) / static_cast<float>(left.width - 1);
            left.pixels[offset + 1] = static_cast<float>(y) / static_cast<float>(left.height - 1);
            left.pixels[offset + 2] = 0.25F;
            left.pixels[offset + 3] = 1.0F;
        }
    }
    auto right = left;

    std::vector<photonstack::MosaicProgress> events;
    photonstack::MosaicOptions options;
    options.overlapPixels = 32;
    options.exposureMatching = false;
    options.progress = [&](const photonstack::MosaicProgress& event) {
        events.push_back(event);
    };

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal({left, right}, options);

    assert(result.ok);
    assert(!events.empty());
    assert(std::adjacent_find(events.begin(), events.end(), [](const auto& first, const auto& second) {
        return second.progress < first.progress;
    }) == events.end());
    assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.stage == photonstack::MosaicProgressStage::Blending && event.row > 0;
    }));
    assert(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.stage == photonstack::MosaicProgressStage::Normalizing && event.row > 0;
    }));
    assert(std::fabs(events.back().progress - 1.0) < 1.0e-9);
}

void testMosaicBuilderGridLayout() {
    std::vector<photonstack::ImageBuffer> images;
    for (int value = 1; value <= 4; ++value) {
        photonstack::ImageBuffer image;
        image.width = 2;
        image.height = 2;
        image.channels = 4;
        image.pixels.assign(image.sampleCount(), 0.0F);
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            image.pixels[pixel * image.channels] = static_cast<float>(value) / 4.0F;
            image.pixels[pixel * image.channels + 3] = 1.0F;
        }
        images.push_back(image);
    }

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal(
        images,
        {.overlapPixels = 0, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Grid, .alignment = photonstack::MosaicAlignment::Manual, .columns = 2});

    assert(result.ok);
    assert(result.image.width == 4);
    assert(result.image.height == 4);
    assert(result.image.pixels[0] < result.image.pixels[(static_cast<std::size_t>(3) * result.image.width + 3) * 4]);
}

void testMosaicBuilderAutoAlignment() {
    photonstack::ImageBuffer left;
    left.width = 12;
    left.height = 8;
    left.channels = 4;
    left.pixels.assign(left.sampleCount(), 0.0F);

    photonstack::ImageBuffer right = left;
    makeOpaque(left);
    makeOpaque(right);

    const auto setPixel = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
        image.pixels[offset + 3] = 1.0F;
    };

    setPixel(left, 9, 2, 1.0F);
    setPixel(left, 10, 5, 0.9F);
    setPixel(right, 1, 2, 1.0F);
    setPixel(right, 2, 5, 0.9F);

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal(
        {left, right},
        {.overlapPixels = 2, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Auto,
         .columns = 2, .registration = {.starDetection = {.sigmaThreshold = 2.0F, .minPeak = 0.2F}, .matchTolerance = 2.0F}});

    assert(result.ok);
    assert(result.usedAutoAlignment);
    assert(result.matchedPairs >= 2);
    assert(result.image.width == 20);
    assert(result.fallbackPanels == 0);
    assert(result.placements[1].usedReducedModel);
    assert(result.placements[1].transformModel == photonstack::MosaicTransformModel::Similarity ||
           result.placements[1].transformModel == photonstack::MosaicTransformModel::Translation);
}

void testMosaicBuilderAutoAffineGeometry() {
    constexpr float a = 1.012F;
    constexpr float b = -0.025F;
    constexpr float c = 0.032F;
    constexpr float d = 0.995F;
    constexpr float dx = 72.0F;
    constexpr float dy = -1.5F;

    photonstack::ImageBuffer reference;
    reference.width = 180;
    reference.height = 120;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.01F);
    photonstack::ImageBuffer moving = reference;
    for (std::size_t pixel = 0; pixel < reference.pixelCount(); ++pixel) {
        reference.pixels[pixel * reference.channels + 3] = 1.0F;
        moving.pixels[pixel * moving.channels + 3] = 1.0F;
    }

    struct StarSpec {
        float x;
        float y;
        float peak;
    };
    const std::array<StarSpec, 12> stars = {{
        {8.0F, 12.0F, 0.95F}, {22.0F, 31.0F, 0.72F}, {38.0F, 17.0F, 0.84F},
        {49.0F, 55.0F, 0.64F}, {61.0F, 88.0F, 0.91F}, {74.0F, 42.0F, 0.78F},
        {86.0F, 69.0F, 0.68F}, {95.0F, 104.0F, 0.88F}, {31.0F, 98.0F, 0.74F},
        {67.0F, 16.0F, 0.82F}, {14.0F, 73.0F, 0.66F}, {91.0F, 29.0F, 0.76F},
    }};
    for (const auto& star : stars) {
        const float referenceX = a * star.x + b * star.y + dx;
        const float referenceY = c * star.x + d * star.y + dy;
        addGaussianStar(moving, star.x, star.y, star.peak);
        addGaussianStar(reference, referenceX, referenceY, star.peak);
    }

    photonstack::MosaicOptions options;
    options.overlapPixels = 72;
    options.alignment = photonstack::MosaicAlignment::Auto;
    options.exposureMatching = false;
    options.registration.starDetection.sigmaThreshold = 1.5F;
    options.registration.starDetection.minPeak = 0.1F;
    options.registration.matchTolerance = 2.5F;
    options.registration.minimumMatches = 6;

    const photonstack::MosaicBuilder builder;
    for (const auto blendMode : {
             photonstack::MosaicBlendMode::Average,
             photonstack::MosaicBlendMode::Feather,
             photonstack::MosaicBlendMode::Multiband,
         }) {
        options.blendMode = blendMode;
        const auto result = builder.stitchHorizontal({reference, moving}, options);
        assert(result.ok);
        assert(result.usedAutoAlignment);
        assert(result.fallbackPanels == 0);
        assert(result.placements.size() == 2);
        const auto& anchor = result.placements[0];
        const auto& placement = result.placements[1];
        assert(placement.transformModel == photonstack::MosaicTransformModel::Affine);
        assert(!placement.usedReducedModel);
        assert(placement.matches >= 6);
        assert(std::fabs(placement.a - a) < 0.025F);
        assert(std::fabs(placement.b - b) < 0.025F);
        assert(std::fabs(placement.c - c) < 0.025F);
        assert(std::fabs(placement.d - d) < 0.025F);

        for (const auto& star : stars) {
            const float referenceX = a * star.x + b * star.y + dx;
            const float referenceY = c * star.x + d * star.y + dy;
            const float globalReferenceX = anchor.a * referenceX + anchor.b * referenceY + anchor.x;
            const float globalReferenceY = anchor.c * referenceX + anchor.d * referenceY + anchor.y;
            const float globalMovingX = placement.a * star.x + placement.b * star.y + placement.x;
            const float globalMovingY = placement.c * star.x + placement.d * star.y + placement.y;
            assert(std::hypot(globalMovingX - globalReferenceX, globalMovingY - globalReferenceY) < 1.0F);
            const int sampleX = static_cast<int>(std::lround(globalReferenceX));
            const int sampleY = static_cast<int>(std::lround(globalReferenceY));
            assert(sampleX >= 0 && sampleX < static_cast<int>(result.image.width));
            assert(sampleY >= 0 && sampleY < static_cast<int>(result.image.height));
            const auto offset =
                (static_cast<std::size_t>(sampleY) * result.image.width + static_cast<std::uint32_t>(sampleX)) *
                result.image.channels;
            assert(result.image.pixels[offset] > 0.30F);
        }

        for (const auto corner : std::array<std::pair<float, float>, 4>{
                 std::pair{0.0F, 0.0F},
                 std::pair{static_cast<float>(moving.width - 1), 0.0F},
                 std::pair{0.0F, static_cast<float>(moving.height - 1)},
                 std::pair{static_cast<float>(moving.width - 1), static_cast<float>(moving.height - 1)},
             }) {
            const float globalX = placement.a * corner.first + placement.b * corner.second + placement.x;
            const float globalY = placement.c * corner.first + placement.d * corner.second + placement.y;
            assert(globalX >= -1.0e-3F && globalX <= static_cast<float>(result.image.width - 1) + 1.0e-3F);
            assert(globalY >= -1.0e-3F && globalY <= static_cast<float>(result.image.height - 1) + 1.0e-3F);
        }
    }

    options.blendMode = photonstack::MosaicBlendMode::Average;
    options.alignmentWidth = 140;
    const auto coarseResult = builder.stitchHorizontal({reference, moving}, options);
    assert(coarseResult.ok);
    assert(coarseResult.usedAutoAlignment);
    assert(coarseResult.placements[1].usedCoarseAlignment);
    assert(std::fabs(coarseResult.placements[1].a - a) < 0.035F);
    assert(std::fabs(coarseResult.placements[1].b - b) < 0.035F);
    assert(std::fabs(coarseResult.placements[1].c - c) < 0.035F);
    assert(std::fabs(coarseResult.placements[1].d - d) < 0.035F);

    const photonstack::ImageCodec codec;
    const auto referencePath = std::filesystem::temp_directory_path() / "photonstack-mosaic-affine-reference.png";
    const auto movingPath = std::filesystem::temp_directory_path() / "photonstack-mosaic-affine-moving.png";
    assert(writeSRGBFixture(codec, reference, referencePath).ok);
    assert(writeSRGBFixture(codec, moving, movingPath).ok);
    options.previewWidth = 90;
    options.alignmentWidth = 140;
    const auto previewResult = builder.stitchHorizontal({referencePath, movingPath}, options);
    assert(previewResult.ok);
    assert(previewResult.usedAutoAlignment);
    assert(previewResult.placements[1].usedCoarseAlignment);
    assert(std::fabs(previewResult.placements[1].a - a) < 0.035F);
    assert(std::fabs(previewResult.placements[1].b - b) < 0.035F);
    assert(std::fabs(previewResult.placements[1].c - c) < 0.035F);
    assert(std::fabs(previewResult.placements[1].d - d) < 0.035F);
    std::filesystem::remove(referencePath);
    std::filesystem::remove(movingPath);
}

void testMosaicBuilderAutoAffineExposureMatching() {
    constexpr float a = 1.008F;
    constexpr float b = -0.018F;
    constexpr float c = 0.026F;
    constexpr float d = 0.997F;
    constexpr float dx = 70.0F;
    constexpr float dy = -1.0F;
    constexpr float exposure = 1.4F;
    const auto scene = [](float x, float y) {
        return std::clamp(0.08F + 0.0018F * x + 0.0009F * y, 0.0F, 0.85F);
    };

    photonstack::ImageBuffer reference;
    reference.width = 180;
    reference.height = 120;
    reference.channels = 4;
    reference.pixels.assign(reference.sampleCount(), 0.0F);
    photonstack::ImageBuffer moving = reference;
    for (std::uint32_t y = 0; y < reference.height; ++y) {
        for (std::uint32_t x = 0; x < reference.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * reference.width + x) * reference.channels;
            const float referenceValue = scene(static_cast<float>(x), static_cast<float>(y));
            const float mappedX = a * static_cast<float>(x) + b * static_cast<float>(y) + dx;
            const float mappedY = c * static_cast<float>(x) + d * static_cast<float>(y) + dy;
            const float movingValue = scene(mappedX, mappedY) / exposure;
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                reference.pixels[offset + channel] = referenceValue;
                moving.pixels[offset + channel] = movingValue;
            }
            reference.pixels[offset + 3] = 1.0F;
            moving.pixels[offset + 3] = 1.0F;
        }
    }

    const std::array<std::pair<float, float>, 10> stars = {{
        {10.0F, 15.0F}, {24.0F, 38.0F}, {39.0F, 20.0F}, {52.0F, 61.0F}, {65.0F, 93.0F},
        {78.0F, 46.0F}, {90.0F, 75.0F}, {98.0F, 108.0F}, {34.0F, 102.0F}, {72.0F, 18.0F},
    }};
    for (std::size_t index = 0; index < stars.size(); ++index) {
        const float peak = 0.72F + 0.02F * static_cast<float>(index);
        const float referenceX = a * stars[index].first + b * stars[index].second + dx;
        const float referenceY = c * stars[index].first + d * stars[index].second + dy;
        addGaussianStar(reference, referenceX, referenceY, peak);
        addGaussianStar(moving, stars[index].first, stars[index].second, peak / exposure);
    }

    photonstack::MosaicOptions options;
    options.overlapPixels = 70;
    options.alignment = photonstack::MosaicAlignment::Auto;
    options.blendMode = photonstack::MosaicBlendMode::Average;
    options.exposureMatching = true;
    options.registration.starDetection.sigmaThreshold = 1.5F;
    options.registration.starDetection.minPeak = 0.1F;
    options.registration.matchTolerance = 2.5F;
    options.registration.minimumMatches = 6;

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal({reference, moving}, options);
    assert(result.ok);
    assert(result.usedAutoAlignment);
    assert(result.exposureMatched);
    assert(result.placements[1].transformModel == photonstack::MosaicTransformModel::Affine);

    constexpr float movingSampleX = 145.0F;
    constexpr float movingSampleY = 58.0F;
    const auto& placement = result.placements[1];
    const int outputX = static_cast<int>(std::lround(
        placement.a * movingSampleX + placement.b * movingSampleY + placement.x));
    const int outputY = static_cast<int>(std::lround(
        placement.c * movingSampleX + placement.d * movingSampleY + placement.y));
    assert(outputX >= 0 && outputX < static_cast<int>(result.image.width));
    assert(outputY >= 0 && outputY < static_cast<int>(result.image.height));
    const auto outputOffset =
        (static_cast<std::size_t>(outputY) * result.image.width + static_cast<std::uint32_t>(outputX)) *
        result.image.channels;
    const float expected = scene(
        a * movingSampleX + b * movingSampleY + dx,
        c * movingSampleX + d * movingSampleY + dy);
    assert(std::fabs(result.image.pixels[outputOffset] - expected) < 0.035F);
}

void testMosaicBuilderAutoGridUsesAvailableNeighbors() {
    struct GlobalStar {
        float x;
        float y;
        float peak;
    };
    std::vector<GlobalStar> stars;
    std::uint32_t state = 0x7A4D21C3U;
    for (int attempt = 0; attempt < 4000 && stars.size() < 125; ++attempt) {
        state = state * 1664525U + 1013904223U;
        const float x = 6.0F + static_cast<float>((state >> 8U) % 268U);
        state = state * 1664525U + 1013904223U;
        const float y = 6.0F + static_cast<float>((state >> 8U) % 188U);
        const bool separated = std::all_of(stars.begin(), stars.end(), [x, y](const GlobalStar& star) {
            return std::hypot(star.x - x, star.y - y) >= 7.0F;
        });
        if (!separated) {
            continue;
        }
        state = state * 1664525U + 1013904223U;
        stars.push_back({
            .x = x,
            .y = y,
            .peak = 0.58F + 0.003F * static_cast<float>((state >> 12U) % 120U),
        });
    }
    assert(stars.size() >= 100);

    const auto makePanel = [&stars](float originX, float originY, float exposure) {
        photonstack::ImageBuffer image;
        image.width = 180;
        image.height = 130;
        image.channels = 4;
        image.pixels.assign(image.sampleCount(), 0.01F);
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            image.pixels[pixel * image.channels + 3] = 1.0F;
        }
        for (const auto& star : stars) {
            const float localX = star.x - originX;
            const float localY = star.y - originY;
            if (localX >= 4.0F && localX < static_cast<float>(image.width) - 4.0F &&
                localY >= 4.0F && localY < static_cast<float>(image.height) - 4.0F) {
                addGaussianStar(image, localX, localY, star.peak / exposure);
            }
        }
        return image;
    };

    auto topLeft = makePanel(0.0F, 0.0F, 1.0F);
    auto topRight = makePanel(100.0F, 0.0F, 1.15F);
    auto bottomLeft = makePanel(0.0F, 70.0F, 0.9F);
    auto bottomRight = makePanel(100.0F, 70.0F, 1.25F);

    // Keep enough of panel 2 for its vertical registration, but remove the stars
    // in its right overlap so panel 3 must use its valid upper neighbor.
    for (std::uint32_t y = 0; y < bottomLeft.height; ++y) {
        for (std::uint32_t x = 88; x < bottomLeft.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * bottomLeft.width + x) * bottomLeft.channels;
            bottomLeft.pixels[offset] = 0.01F;
            bottomLeft.pixels[offset + 1] = 0.01F;
            bottomLeft.pixels[offset + 2] = 0.01F;
        }
    }

    photonstack::MosaicOptions options;
    options.overlapPixels = 80;
    options.layout = photonstack::MosaicLayout::Grid;
    options.columns = 2;
    options.alignment = photonstack::MosaicAlignment::Auto;
    options.blendMode = photonstack::MosaicBlendMode::Feather;
    options.exposureMatching = true;
    options.registration.starDetection.sigmaThreshold = 1.5F;
    options.registration.starDetection.minPeak = 0.1F;
    options.registration.matchTolerance = 2.5F;
    options.registration.minimumMatches = 6;

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal({topLeft, topRight, bottomLeft, bottomRight}, options);
    assert(result.ok);
    assert(result.usedAutoAlignment);
    assert(result.fallbackPanels == 0);
    assert(result.placements[3].autoAligned);
    assert(!result.placements[3].usedFallback);
    assert(std::fabs((result.placements[3].x - result.placements[0].x) - 100.0F) < 3.0F);
    assert(std::fabs((result.placements[3].y - result.placements[0].y) - 70.0F) < 3.0F);

    options.projection = photonstack::MosaicProjection::Cylindrical;
    const auto cylindrical = builder.stitchHorizontal({topLeft, topRight, makePanel(0.0F, 70.0F, 0.9F), bottomRight}, options);
    assert(cylindrical.ok);
    assert(cylindrical.usedAutoAlignment);
    assert(cylindrical.fallbackPanels == 0);
    assert(cylindrical.placements[3].autoAligned);

    struct PanelGeometry {
        float a;
        float b;
        float c;
        float d;
        float x;
        float y;
    };
    const auto makeAffinePanel = [&stars](const PanelGeometry& geometry, float exposure) {
        photonstack::ImageBuffer image;
        image.width = 180;
        image.height = 130;
        image.channels = 4;
        image.pixels.assign(image.sampleCount(), 0.01F);
        for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
            image.pixels[pixel * image.channels + 3] = 1.0F;
        }
        const float determinant = geometry.a * geometry.d - geometry.b * geometry.c;
        for (const auto& star : stars) {
            const float translatedX = star.x - geometry.x;
            const float translatedY = star.y - geometry.y;
            const float localX = (geometry.d * translatedX - geometry.b * translatedY) / determinant;
            const float localY = (-geometry.c * translatedX + geometry.a * translatedY) / determinant;
            if (localX >= 4.0F && localX < static_cast<float>(image.width) - 4.0F &&
                localY >= 4.0F && localY < static_cast<float>(image.height) - 4.0F) {
                addGaussianStar(image, localX, localY, star.peak / exposure);
            }
        }
        return image;
    };
    const std::array<PanelGeometry, 4> geometries = {{
        {1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F},
        {1.004F, -0.010F, 0.008F, 0.999F, 100.0F, 0.5F},
        {0.998F, 0.009F, -0.006F, 1.003F, 0.5F, 70.0F},
        {1.003F, -0.008F, 0.006F, 0.999F, 100.5F, 70.5F},
    }};
    options.projection = photonstack::MosaicProjection::Planar;
    const auto affineGrid = builder.stitchHorizontal(
        {
            makeAffinePanel(geometries[0], 1.0F),
            makeAffinePanel(geometries[1], 1.15F),
            makeAffinePanel(geometries[2], 0.9F),
            makeAffinePanel(geometries[3], 1.25F),
        },
        options);
    assert(affineGrid.ok);
    assert(affineGrid.fallbackPanels == 0);
    const auto& affinePlacement = affineGrid.placements[3];
    assert(affinePlacement.autoAligned);
    assert(affinePlacement.referenceCount == 2);
    assert(std::fabs(affinePlacement.a - geometries[3].a) < 0.035F);
    assert(std::fabs(affinePlacement.b - geometries[3].b) < 0.035F);
    assert(std::fabs(affinePlacement.c - geometries[3].c) < 0.035F);
    assert(std::fabs(affinePlacement.d - geometries[3].d) < 0.035F);
    assert(std::fabs((affinePlacement.x - affineGrid.placements[0].x) - geometries[3].x) < 3.0F);
    assert(std::fabs((affinePlacement.y - affineGrid.placements[0].y) - geometries[3].y) < 3.0F);
}

void testMosaicBuilderPropagatesFallbackTrust() {
    photonstack::ImageBuffer blank;
    blank.width = 180;
    blank.height = 120;
    blank.channels = 4;
    blank.pixels.assign(blank.sampleCount(), 0.01F);
    for (std::size_t pixel = 0; pixel < blank.pixelCount(); ++pixel) {
        blank.pixels[pixel * blank.channels + 3] = 1.0F;
    }
    auto middle = blank;
    auto right = blank;
    const std::array<std::pair<float, float>, 10> stars = {{
        {92.0F, 12.0F}, {105.0F, 28.0F}, {118.0F, 18.0F}, {132.0F, 47.0F}, {146.0F, 83.0F},
        {158.0F, 61.0F}, {101.0F, 99.0F}, {124.0F, 107.0F}, {151.0F, 34.0F}, {166.0F, 94.0F},
    }};
    for (std::size_t index = 0; index < stars.size(); ++index) {
        const float peak = 0.65F + 0.025F * static_cast<float>(index);
        addGaussianStar(middle, stars[index].first, stars[index].second, peak);
        addGaussianStar(right, stars[index].first - 80.0F, stars[index].second, peak);
    }

    photonstack::MosaicOptions options;
    options.overlapPixels = 80;
    options.alignment = photonstack::MosaicAlignment::Auto;
    options.exposureMatching = false;
    options.registration.starDetection.sigmaThreshold = 1.5F;
    options.registration.starDetection.minPeak = 0.1F;
    options.registration.matchTolerance = 2.5F;
    options.registration.minimumMatches = 6;

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal({blank, middle, right}, options);
    assert(result.ok);
    assert(result.placements[1].usedFallback);
    assert(result.placements[2].autoAligned);
    assert(result.placements[2].usedFallback);
    assert(result.fallbackPanels == 2);
}

void testMosaicBuilderExposureMatching() {
    photonstack::ImageBuffer left;
    left.width = 8;
    left.height = 4;
    left.channels = 4;
    left.pixels.assign(left.sampleCount(), 0.2F);

    photonstack::ImageBuffer right = left;
    right.pixels.assign(right.sampleCount(), 0.6F);
    for (std::size_t pixel = 0; pixel < left.pixelCount(); ++pixel) {
        left.pixels[pixel * left.channels + 3] = 1.0F;
        right.pixels[pixel * right.channels + 3] = 1.0F;
    }

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal(
        {left, right},
        {.overlapPixels = 4, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = true, .columns = 2});

    assert(result.ok);
    assert(result.exposureMatched);
    const auto rightOnlyOffset = (static_cast<std::size_t>(1) * result.image.width + 10) * result.image.channels;
    assert(std::fabs(result.image.pixels[rightOnlyOffset] - 0.35F) < 0.01F);

    photonstack::ImageBuffer gradientLeft;
    gradientLeft.width = 16;
    gradientLeft.height = 4;
    gradientLeft.channels = 4;
    gradientLeft.pixels.assign(gradientLeft.sampleCount(), 0.0F);
    photonstack::ImageBuffer gradientRight = gradientLeft;
    for (std::uint32_t y = 0; y < gradientLeft.height; ++y) {
        for (std::uint32_t x = 0; x < gradientLeft.width; ++x) {
            const auto leftOffset = (static_cast<std::size_t>(y) * gradientLeft.width + x) * 4;
            const auto rightOffset = (static_cast<std::size_t>(y) * gradientRight.width + x) * 4;
            const float leftScene = 0.1F + 0.02F * static_cast<float>(x);
            const float rightScene = 0.1F + 0.02F * static_cast<float>(x + 8);
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                gradientLeft.pixels[leftOffset + channel] = leftScene;
                gradientRight.pixels[rightOffset + channel] = rightScene / 1.5F;
            }
            gradientLeft.pixels[leftOffset + 3] = 1.0F;
            gradientRight.pixels[rightOffset + 3] = 1.0F;
        }
    }
    const auto gainMatched = builder.stitchHorizontal(
        {gradientLeft, gradientRight},
        {.overlapPixels = 8, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = true, .columns = 2});
    assert(gainMatched.ok);
    assert(gainMatched.exposureMatched);
    const auto gainMatchedOffset = (static_cast<std::size_t>(2) * gainMatched.image.width + 20) * 4;
    assert(std::fabs(gainMatched.image.pixels[gainMatchedOffset] - 0.5F) < 0.01F);

    photonstack::ImageBuffer scientificReference;
    scientificReference.width = 32;
    scientificReference.height = 4;
    scientificReference.channels = 4;
    scientificReference.colorEncoding = photonstack::ColorEncoding::Linear;
    scientificReference.pixels.assign(scientificReference.sampleCount(), 0.0F);
    auto scientificMoving = scientificReference;
    for (std::uint32_t y = 0; y < scientificReference.height; ++y) {
        for (std::uint32_t x = 0; x < scientificReference.width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * scientificReference.width + x) * scientificReference.channels;
            const float referenceScene = 1000.0F + 10.0F * static_cast<float>(x);
            const float movingScene = 1000.0F + 10.0F * static_cast<float>(x + 16);
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                scientificReference.pixels[offset + channel] = referenceScene;
                scientificMoving.pixels[offset + channel] = (movingScene - 100.0F) / 1.5F;
            }
            scientificReference.pixels[offset + 3] = 1.0F;
            scientificMoving.pixels[offset + 3] = 1.0F;
        }
    }
    const photonstack::MosaicOptions scientificOptions{
        .overlapPixels = 16, .projection = photonstack::MosaicProjection::Planar,
        .layout = photonstack::MosaicLayout::Horizontal,
        .alignment = photonstack::MosaicAlignment::Manual,
        .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = true,
        .fitsDecodeMode = photonstack::FitsDecodeMode::Scientific};
    const auto scientificMatched = builder.stitchHorizontal(
        {scientificReference, scientificMoving}, scientificOptions);
    assert(scientificMatched.ok);
    assert(scientificMatched.exposureMatched);
    const auto scientificRightOffset =
        (static_cast<std::size_t>(2) * scientificMatched.image.width + 40) * 4;
    assert(std::fabs(scientificMatched.image.pixels[scientificRightOffset] - 1400.0F) < 0.1F);

    auto tinyReference = scientificReference;
    auto tinyMoving = scientificMoving;
    constexpr float tinyScale = 1.0e-10F;
    for (auto* image : {&tinyReference, &tinyMoving}) {
        for (std::size_t pixel = 0; pixel < image->pixelCount(); ++pixel) {
            const auto offset = pixel * image->channels;
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                image->pixels[offset + channel] *= tinyScale;
            }
        }
    }
    const auto tinyMatched = builder.stitchHorizontal({tinyReference, tinyMoving}, scientificOptions);
    assert(tinyMatched.ok);
    const auto tinyRightOffset = (static_cast<std::size_t>(2) * tinyMatched.image.width + 40) * 4;
    assert(std::fabs(tinyMatched.image.pixels[tinyRightOffset] - 1400.0F * tinyScale) < 2.0e-11F);
}

void testMosaicBuilderPreservesCoverageAndReportsExposure() {
    photonstack::ImageBuffer black;
    black.width = 3;
    black.height = 2;
    black.channels = 4;
    black.pixels.assign(black.sampleCount(), 0.0F);
    for (std::size_t pixel = 0; pixel < black.pixelCount(); ++pixel) {
        black.pixels[pixel * black.channels + 3] = 1.0F;
    }

    const photonstack::MosaicBuilder builder;
    const auto opaqueBlack = builder.stitchHorizontal(
        {black},
        {.projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Feather, .exposureMatching = true});
    assert(opaqueBlack.ok);
    assert(!opaqueBlack.exposureMatched);
    for (std::size_t pixel = 0; pixel < opaqueBlack.image.pixelCount(); ++pixel) {
        assert(opaqueBlack.image.pixels[pixel * opaqueBlack.image.channels + 3] > 0.999F);
    }

    photonstack::ImageBuffer red = black;
    red.width = 2;
    red.height = 1;
    red.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 0.0F, 0.0F, 1.0F,
    };
    photonstack::ImageBuffer transparentGreen = red;
    transparentGreen.pixels = {
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
    };
    const auto transparentOverlap = builder.stitchHorizontal(
        {red, transparentGreen},
        {.overlapPixels = 1, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false});
    assert(transparentOverlap.ok);
    const auto overlapOffset = static_cast<std::size_t>(1) * transparentOverlap.image.channels;
    assert(transparentOverlap.image.pixels[overlapOffset] > 0.999F);
    assert(transparentOverlap.image.pixels[overlapOffset + 1] < 0.001F);
    assert(transparentOverlap.image.pixels[overlapOffset + 3] > 0.999F);
    const auto transparentOnlyOffset = static_cast<std::size_t>(2) * transparentOverlap.image.channels;
    assert(transparentOverlap.image.pixels[transparentOnlyOffset + 3] < 0.001F);

    photonstack::ImageBuffer invalid = black;
    invalid.pixels.clear();
    const auto invalidLaterPanel = builder.stitchHorizontal(
        {black, invalid},
        {.projection = photonstack::MosaicProjection::Cylindrical,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false});
    assert(!invalidLaterPanel.ok);
    assert(invalidLaterPanel.errorCode == "ImageBufferInvalid");

    photonstack::ImageBuffer nonFinite = black;
    nonFinite.pixels.front() = std::numeric_limits<float>::quiet_NaN();
    const auto nonFinitePanel = builder.stitchHorizontal({nonFinite});
    assert(!nonFinitePanel.ok);
    assert(nonFinitePanel.errorCode == "ImageBufferInvalid");
}

void testMosaicCylindricalProjectionIgnoresColorBehindTransparency() {
    photonstack::ImageBuffer image;
    image.width = 4;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        1.0F, 0.0F, 0.0F, 1.0F,
        1.0F, 0.0F, 0.0F, 1.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 0.0F, 1.0F,
    };

    const auto result = photonstack::MosaicBuilder().stitchHorizontal(
        {image},
        {.projection = photonstack::MosaicProjection::Cylindrical,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false});

    assert(result.ok);
    assert(result.image.width == 4);
    assert(result.image.height == 1);
    constexpr std::size_t partialCoverageOffset = 2 * 4;
    assert(result.image.pixels[partialCoverageOffset + 3] > 0.001F);
    assert(result.image.pixels[partialCoverageOffset + 3] < 0.1F);
    assert(result.image.pixels[partialCoverageOffset] > 0.999F);
    assert(result.image.pixels[partialCoverageOffset + 1] < 1.0e-6F);
    assert(result.image.pixels[partialCoverageOffset + 2] < 1.0e-6F);
}

void testMosaicPreviewUsesCommonScale() {
    photonstack::ImageBuffer narrow;
    narrow.width = 100;
    narrow.height = 20;
    narrow.channels = 4;
    narrow.pixels.assign(narrow.sampleCount(), 0.25F);
    photonstack::ImageBuffer wide = narrow;
    wide.width = 200;
    wide.pixels.assign(wide.sampleCount(), 0.5F);
    for (std::size_t pixel = 0; pixel < narrow.pixelCount(); ++pixel) {
        narrow.pixels[pixel * narrow.channels + 3] = 1.0F;
    }
    for (std::size_t pixel = 0; pixel < wide.pixelCount(); ++pixel) {
        wide.pixels[pixel * wide.channels + 3] = 1.0F;
    }

    const photonstack::ImageCodec codec;
    const auto narrowPath = std::filesystem::temp_directory_path() / "photonstack-mosaic-preview-narrow.png";
    const auto widePath = std::filesystem::temp_directory_path() / "photonstack-mosaic-preview-wide.png";
    assert(writeSRGBFixture(codec, narrow, narrowPath).ok);
    assert(writeSRGBFixture(codec, wide, widePath).ok);

    const photonstack::MosaicBuilder builder;
    const auto preview = builder.stitchHorizontal(
        {narrowPath, widePath},
        {.overlapPixels = 20, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false,
         .previewWidth = 100});
    assert(preview.ok);
    assert(preview.image.width == 140);
    assert(preview.image.height == 10);
}

void testMosaicBuilderMultibandBlendMode() {
    photonstack::ImageBuffer left;
    left.width = 4;
    left.height = 4;
    left.channels = 4;
    left.pixels.assign(left.sampleCount(), 0.25F);

    photonstack::ImageBuffer right = left;
    right.pixels.assign(right.sampleCount(), 0.75F);
    for (std::size_t pixel = 0; pixel < left.pixelCount(); ++pixel) {
        left.pixels[pixel * left.channels + 3] = 1.0F;
        right.pixels[pixel * right.channels + 3] = 1.0F;
    }

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal(
        {left, right},
        {.overlapPixels = 2, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Multiband, .exposureMatching = false, .columns = 2});

    assert(result.ok);
    assert(result.blendMode == photonstack::MosaicBlendMode::Multiband);
    assert(result.image.width == 6);

    photonstack::ImageBuffer patterned;
    patterned.width = 32;
    patterned.height = 16;
    patterned.channels = 4;
    patterned.pixels.assign(patterned.sampleCount(), 0.0F);
    for (std::uint32_t y = 0; y < patterned.height; ++y) {
        for (std::uint32_t x = 0; x < patterned.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * patterned.width + x) * patterned.channels;
            const float detail = ((x + y) % 2 == 0) ? 0.12F : -0.12F;
            patterned.pixels[offset] = 0.45F + detail;
            patterned.pixels[offset + 1] = 0.35F + detail * 0.5F;
            patterned.pixels[offset + 2] = 0.25F;
            patterned.pixels[offset + 3] = 1.0F;
        }
    }
    const auto reconstructed = builder.stitchHorizontal(
        {patterned},
        {.projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Manual,
         .blendMode = photonstack::MosaicBlendMode::Multiband, .exposureMatching = false});
    assert(reconstructed.ok);
    assert(reconstructed.image.pixels.size() == patterned.pixels.size());
    float meanAbsoluteError = 0.0F;
    for (std::size_t sample = 0; sample < patterned.pixels.size(); ++sample) {
        meanAbsoluteError += std::fabs(reconstructed.image.pixels[sample] - patterned.pixels[sample]);
    }
    meanAbsoluteError /= static_cast<float>(patterned.pixels.size());
    assert(meanAbsoluteError < 1.0e-4F);
}

void testMosaicBuilderPreservesScientificRangeWithoutOverflow() {
    photonstack::ImageBuffer maximumPanel;
    maximumPanel.width = 4;
    maximumPanel.height = 4;
    maximumPanel.channels = 4;
    maximumPanel.colorEncoding = photonstack::ColorEncoding::Linear;
    maximumPanel.pixels.assign(maximumPanel.sampleCount(), std::numeric_limits<float>::max());
    for (std::size_t pixel = 0; pixel < maximumPanel.pixelCount(); ++pixel) {
        maximumPanel.pixels[pixel * maximumPanel.channels + 3] = 1.0F;
    }

    const photonstack::MosaicBuilder builder;
    for (const auto blendMode : {photonstack::MosaicBlendMode::Average,
                                 photonstack::MosaicBlendMode::Feather,
                                 photonstack::MosaicBlendMode::Multiband}) {
        const auto result = builder.stitchHorizontal(
            {maximumPanel, maximumPanel},
            {.overlapPixels = 3, .projection = photonstack::MosaicProjection::Planar,
             .layout = photonstack::MosaicLayout::Horizontal,
             .alignment = photonstack::MosaicAlignment::Manual,
             .blendMode = blendMode, .exposureMatching = false,
             .fitsDecodeMode = photonstack::FitsDecodeMode::Scientific});
        assert(result.ok);
        for (std::size_t pixel = 0; pixel < result.image.pixelCount(); ++pixel) {
            const auto offset = pixel * result.image.channels;
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                assert(std::isfinite(result.image.pixels[offset + channel]));
                assert(result.image.pixels[offset + channel] >
                       std::numeric_limits<float>::max() * 0.99F);
            }
        }
    }

    photonstack::ImageBuffer left;
    left.width = 16;
    left.height = 8;
    left.channels = 4;
    left.colorEncoding = photonstack::ColorEncoding::Linear;
    left.pixels.assign(left.sampleCount(), 0.0F);
    photonstack::ImageBuffer right = left;
    for (std::uint32_t y = 0; y < left.height; ++y) {
        for (std::uint32_t x = 0; x < left.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * left.width + x) * left.channels;
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                left.pixels[offset + channel] =
                    0.08F + 0.015F * static_cast<float>(x) + 0.01F * static_cast<float>(y) +
                    0.025F * static_cast<float>(channel);
                right.pixels[offset + channel] =
                    0.18F + 0.012F * static_cast<float>(x) + 0.006F * static_cast<float>(y) +
                    0.02F * static_cast<float>(channel);
            }
            left.pixels[offset + 3] = 1.0F;
            right.pixels[offset + 3] = 1.0F;
        }
    }

    constexpr float scale = 4.25F;
    constexpr float offset = -2.0F;
    auto scientificLeft = left;
    auto scientificRight = right;
    for (auto* image : {&scientificLeft, &scientificRight}) {
        for (std::size_t pixel = 0; pixel < image->pixelCount(); ++pixel) {
            const auto sampleOffset = pixel * image->channels;
            for (std::uint16_t channel = 0; channel < 3; ++channel) {
                image->pixels[sampleOffset + channel] =
                    image->pixels[sampleOffset + channel] * scale + offset;
            }
        }
    }
    const photonstack::MosaicOptions options{
        .overlapPixels = 8, .projection = photonstack::MosaicProjection::Planar,
        .layout = photonstack::MosaicLayout::Horizontal,
        .alignment = photonstack::MosaicAlignment::Manual,
        .blendMode = photonstack::MosaicBlendMode::Multiband, .exposureMatching = false,
        .fitsDecodeMode = photonstack::FitsDecodeMode::Scientific};
    const auto base = builder.stitchHorizontal({left, right}, options);
    const auto scientific = builder.stitchHorizontal({scientificLeft, scientificRight}, options);
    assert(base.ok);
    assert(scientific.ok);
    assert(base.image.pixels.size() == scientific.image.pixels.size());
    for (std::size_t pixel = 0; pixel < base.image.pixelCount(); ++pixel) {
        const auto sampleOffset = pixel * base.image.channels;
        for (std::uint16_t channel = 0; channel < 3; ++channel) {
            const float expected = base.image.pixels[sampleOffset + channel] * scale + offset;
            assert(std::fabs(scientific.image.pixels[sampleOffset + channel] - expected) < 2.0e-4F);
        }
    }
}

void testMosaicBuilderKeepsSubpixelPlacement() {
    photonstack::ImageBuffer left;
    left.width = 64;
    left.height = 32;
    left.channels = 4;
    left.pixels.assign(left.sampleCount(), 0.0F);
    photonstack::ImageBuffer right = left;
    for (std::size_t pixel = 0; pixel < left.pixelCount(); ++pixel) {
        left.pixels[pixel * left.channels + 3] = 1.0F;
        right.pixels[pixel * right.channels + 3] = 1.0F;
    }
    const auto setStar = [](photonstack::ImageBuffer& image, std::uint32_t x, std::uint32_t y, float value) {
        const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
        image.pixels[offset] = value;
        image.pixels[offset + 1] = value;
        image.pixels[offset + 2] = value;
    };
    for (const auto y : {6U, 13U, 21U, 27U}) {
        setStar(left, 40, y, 0.75F);
        setStar(left, 41, y, 0.25F);
        setStar(right, 8, y, 1.0F);
    }

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal(
        {left, right},
        {.overlapPixels = 32, .projection = photonstack::MosaicProjection::Planar,
         .layout = photonstack::MosaicLayout::Horizontal, .alignment = photonstack::MosaicAlignment::Auto,
         .blendMode = photonstack::MosaicBlendMode::Average, .exposureMatching = false, .columns = 2,
         .registration = {.starDetection = {.sigmaThreshold = 1.5F, .minPeak = 0.1F}, .matchTolerance = 2.0F}});
    assert(result.ok);
    assert(result.usedAutoAlignment);
    assert(result.placements.size() == 2);
    const float fractionalX = result.placements[1].x - std::floor(result.placements[1].x);
    assert(fractionalX > 0.1F && fractionalX < 0.9F);
    const auto alignedPeak = (static_cast<std::size_t>(13) * result.image.width + 40) * result.image.channels;
    const auto alignedShoulder = (static_cast<std::size_t>(13) * result.image.width + 41) * result.image.channels;
    assert(std::fabs(result.image.pixels[alignedPeak] - 0.75F) < 0.03F);
    assert(std::fabs(result.image.pixels[alignedShoulder] - 0.25F) < 0.03F);
}

void testBridgeMosaicPaths() {
    photonstack::ImageBuffer left;
    left.width = 2;
    left.height = 1;
    left.channels = 4;
    left.pixels = {0.1F, 0.1F, 0.1F, 1.0F, 0.2F, 0.2F, 0.2F, 1.0F};

    photonstack::ImageBuffer right;
    right.width = 2;
    right.height = 1;
    right.channels = 4;
    right.pixels = {0.3F, 0.3F, 0.3F, 1.0F, 0.4F, 0.4F, 0.4F, 1.0F};

    const auto leftPath = std::filesystem::temp_directory_path() / "photonstack-bridge-left.png";
    const auto rightPath = std::filesystem::temp_directory_path() / "photonstack-bridge-right.png";
    const auto outputPath = std::filesystem::temp_directory_path() / "photonstack-bridge-mosaic.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, left, leftPath).ok);
    assert(writeSRGBFixture(codec, right, rightPath).ok);

    const std::string leftString = leftPath.string();
    const std::string rightString = rightPath.string();
    const std::string outputString = outputPath.string();
    const char* inputs[] = {leftString.c_str(), rightString.c_str()};
    auto request = photonstack_default_mosaic_request();
    request.overlapPixels = 1;
    request.blendMode = PHOTONSTACK_MOSAIC_BLEND_AVERAGE;
    request.exposureMatching = 0;
    char error[256] = "stale error";
    const auto result = photonstack_mosaic_paths(inputs, 2, outputString.c_str(), request, error, sizeof(error));

    assert(result.ok == 1);
    assert(result.width == 3);
    assert(result.height == 1);
    assert(error[0] == '\0');

    auto invalidRequest = request;
    invalidRequest.blendMode = static_cast<PhotonStackMosaicBlendMode>(99);
    char invalidError[64] = {};
    const auto invalidResult = photonstack_mosaic_paths(
        inputs, 2, outputString.c_str(), invalidRequest, invalidError, sizeof(invalidError));
    assert(invalidResult.ok == 0);
    assert(std::string(invalidError).find("invalid enum") != std::string::npos);

    char countError[64] = {};
    const auto excessiveCount = photonstack_mosaic_paths(
        inputs, std::numeric_limits<std::size_t>::max(), outputString.c_str(), request,
        countError, sizeof(countError));
    assert(excessiveCount.ok == 0);
    assert(std::string(countError).find("count") != std::string::npos);

    const char* emptyInputs[] = {""};
    char emptyError[64] = {};
    const auto emptyInput = photonstack_mosaic_paths(
        emptyInputs, 1, outputString.c_str(), request, emptyError, sizeof(emptyError));
    assert(emptyInput.ok == 0);
    assert(std::string(emptyError).find("must not be empty") != std::string::npos);

    char oneByteError[1] = {'x'};
    const auto missingInputs = photonstack_mosaic_paths(
        nullptr, 0, outputString.c_str(), request, oneByteError, sizeof(oneByteError));
    assert(missingInputs.ok == 0);
    assert(oneByteError[0] == '\0');
}

void testBridgeCurvePreviewPath() {
    photonstack::ImageBuffer input;
    input.width = 2;
    input.height = 1;
    input.channels = 4;
    input.colorEncoding = photonstack::ColorEncoding::SRGB;
    input.pixels = {
        0.25F, 0.25F, 0.25F, 1.0F,
        0.75F, 0.75F, 0.75F, 1.0F,
    };

    const auto inputPath = std::filesystem::temp_directory_path() / "photonstack-bridge-curve-input.png";
    const auto outputPath = std::filesystem::temp_directory_path() / "photonstack-bridge-curve-output.png";
    const photonstack::ImageCodec codec;
    assert(writeSRGBFixture(codec, input, inputPath).ok);

    const std::string inputString = inputPath.string();
    const std::string outputString = outputPath.string();
    const PhotonStackCurvePoint points[] = {
        {.input = 0.0F, .output = 0.0F},
        {.input = 0.5F, .output = 0.25F},
        {.input = 1.0F, .output = 1.0F},
    };
    auto request = photonstack_default_curve_preview_request();
    request.width = 2;
    request.curveChannel = PHOTONSTACK_CURVE_CHANNEL_RGB;
    request.curvePoints = points;
    request.curvePointCount = std::size(points);
    char fallbackCode[64] = {};
    char fallbackMessage[256] = {};
    char error[256] = "stale error";
    const auto result = photonstack_curve_preview_path(
        inputString.c_str(),
        outputString.c_str(),
        request,
        fallbackCode,
        sizeof(fallbackCode),
        fallbackMessage,
        sizeof(fallbackMessage),
        error,
        sizeof(error)
    );

    assert(result.ok == 1);
    assert(result.width == 2);
    assert(result.height == 1);
    assert(result.decodedWidth == 2);
    assert(result.decodedHeight == 1);
    assert(result.backend == PHOTONSTACK_IMAGE_READ_BACKEND_IMAGEIO);
    assert(result.usedFallback == 0);
    assert(error[0] == '\0');
    assert(fallbackCode[0] == '\0');
    assert(fallbackMessage[0] == '\0');

    const auto outputRead = codec.read(outputPath);
    assert(outputRead.ok);
    assert(outputRead.image.width == 2);
    assert(outputRead.image.height == 1);
    assert(outputRead.image.pixels[0] < input.pixels[0]);
    assert(outputRead.image.pixels[4] < input.pixels[4]);

    const auto proxyOutputPath = std::filesystem::temp_directory_path() / "photonstack-bridge-curve-proxy.png";
    const auto proxyOutputString = proxyOutputPath.string();
    request.width = 1;
    request.retainDecodedSource = 1;
    const auto proxyResult = photonstack_curve_preview_path(
        inputString.c_str(),
        proxyOutputString.c_str(),
        request,
        fallbackCode,
        sizeof(fallbackCode),
        fallbackMessage,
        sizeof(fallbackMessage),
        error,
        sizeof(error)
    );
    assert(proxyResult.ok == 1);
    assert(proxyResult.width == 1);
    assert(proxyResult.decodedWidth == 1);

    request.width = 2;
    request.retainDecodedSource = 0;
    const auto fullAfterProxyResult = photonstack_curve_preview_path(
        inputString.c_str(),
        outputString.c_str(),
        request,
        fallbackCode,
        sizeof(fallbackCode),
        fallbackMessage,
        sizeof(fallbackMessage),
        error,
        sizeof(error)
    );
    assert(fullAfterProxyResult.ok == 1);
    assert(fullAfterProxyResult.decodedWidth == 2);
    photonstack_clear_curve_preview_cache();
}

void testWriteTiff16() {
    photonstack::ImageBuffer image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.pixels = {0.5F, 0.25F, 0.75F, 1.0F};

    const auto path = std::filesystem::temp_directory_path() / "photonstack-write-16.tiff";

    const photonstack::ImageCodec codec;
    assert(codec.write(image, path).ok);

    const photonstack::ImageInspector inspector;
    const auto result = inspector.inspect(path);

    assert(result.ok);
    assert(result.metadata.format == photonstack::ImageFormat::TIFF);
    assert(result.metadata.width == 1);
    assert(result.metadata.height == 1);
    assert(result.metadata.bitsPerChannel == 16);
}

void testWriteOptionsCanForceTiff8Bit() {
    photonstack::ImageBuffer image;
    image.width = 1;
    image.height = 1;
    image.channels = 4;
    image.pixels = {0.5F, 0.25F, 0.75F, 1.0F};

    const auto path = std::filesystem::temp_directory_path() / "photonstack-write-8.tiff";

    const photonstack::ImageCodec codec;
    assert(codec.write(image, path, {.bitDepth = photonstack::ImageWriteBitDepth::Eight,
                                     .colorSpace = photonstack::ImageWriteColorSpace::SRGB,
                                     .quality = 0.9F})
               .ok);

    const photonstack::ImageInspector inspector;
    const auto result = inspector.inspect(path);

    assert(result.ok);
    assert(result.metadata.format == photonstack::ImageFormat::TIFF);
    assert(result.metadata.width == 1);
    assert(result.metadata.height == 1);
    assert(result.metadata.bitsPerChannel == 8);
}

void testImageCodecStreamsRowsWithoutChangingPixelsOrLeavingCache() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 3;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::Linear;
    image.sourceBitsPerChannel = 16;
    image.pixels.resize(image.sampleCount());
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        image.pixels[offset] = static_cast<float>((pixel * 7) % 17) / 16.0F;
        image.pixels[offset + 1] = static_cast<float>((pixel * 5 + 2) % 19) / 18.0F;
        image.pixels[offset + 2] = static_cast<float>((pixel * 3 + 1) % 13) / 12.0F;
        image.pixels[offset + 3] = pixel % 4 == 0 ? 0.5F : 1.0F;
    }

    const auto fullPath = std::filesystem::temp_directory_path() / "photonstack-write-full.tiff";
    const auto streamedPath = std::filesystem::temp_directory_path() / "photonstack-write-rows.tiff";
    const auto fullPngPath = std::filesystem::temp_directory_path() / "photonstack-write-full.png";
    const auto streamedPngPath = std::filesystem::temp_directory_path() / "photonstack-write-rows.png";
    const auto failedPath = std::filesystem::temp_directory_path() / "photonstack-write-rows-failed.tiff";
    std::filesystem::remove(failedPath);
    const auto cachesBefore = imageRowCacheDirectories();
    const photonstack::ImageCodec codec;
    assert(codec.write(image, fullPath).ok);
    const auto streamed = codec.writeRows(image, streamedPath, [&](const photonstack::ImageRowConsumer& consumer) {
        const auto rowSamples = static_cast<std::size_t>(image.width) * image.channels;
        for (std::uint32_t row = 0; row < image.height; ++row) {
            if (!consumer(row, image.pixels.data() + static_cast<std::size_t>(row) * rowSamples, rowSamples)) {
                return false;
            }
        }
        return true;
    });
    assert(streamed.ok);
    const auto fullRead = codec.read(fullPath);
    const auto streamedRead = codec.read(streamedPath);
    assert(fullRead.ok);
    assert(streamedRead.ok);
    assert(fullRead.image.sourceBitsPerChannel == 16);
    assert(streamedRead.image.sourceBitsPerChannel == 16);
    assert(fullRead.image.pixels == streamedRead.image.pixels);
    assert(imageRowCacheDirectories() == cachesBefore);

    assert(codec.write(image, fullPngPath).ok);
    const auto streamedPng = codec.writeRows(image, streamedPngPath, [&](const photonstack::ImageRowConsumer& consumer) {
        const auto rowSamples = static_cast<std::size_t>(image.width) * image.channels;
        for (std::uint32_t row = 0; row < image.height; ++row) {
            if (!consumer(row, image.pixels.data() + static_cast<std::size_t>(row) * rowSamples, rowSamples)) {
                return false;
            }
        }
        return true;
    });
    assert(streamedPng.ok);
    const auto fullPngRead = codec.read(fullPngPath);
    const auto streamedPngRead = codec.read(streamedPngPath);
    assert(fullPngRead.ok);
    assert(streamedPngRead.ok);
    assert(fullPngRead.image.sourceBitsPerChannel == 8);
    assert(streamedPngRead.image.sourceBitsPerChannel == 8);
    assert(fullPngRead.image.pixels == streamedPngRead.image.pixels);
    assert(imageRowCacheDirectories() == cachesBefore);

    const auto failed = codec.writeRows(image, failedPath, [&](const photonstack::ImageRowConsumer& consumer) {
        const auto rowSamples = static_cast<std::size_t>(image.width) * image.channels;
        return consumer(0, image.pixels.data(), rowSamples) && false;
    });
    assert(!failed.ok);
    assert(failed.errorCode == "ImageRowProductionFailed");
    assert(!std::filesystem::exists(failedPath));
    assert(imageRowCacheDirectories() == cachesBefore);
}

void testImageCodecReadReportsDecodedBitDepth() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.1234F, 0.2345F, 0.3456F, 1.0F,
        0.4567F, 0.5678F, 0.6789F, 1.0F,
    };

    const auto eightBitPath = std::filesystem::temp_directory_path() / "photonstack-read-depth-8.tiff";
    const auto sixteenBitPath = std::filesystem::temp_directory_path() / "photonstack-read-depth-16.tiff";
    const photonstack::ImageCodec codec;
    assert(codec.write(image, eightBitPath, {.bitDepth = photonstack::ImageWriteBitDepth::Eight}).ok);
    assert(codec.write(image, sixteenBitPath, {.bitDepth = photonstack::ImageWriteBitDepth::Sixteen}).ok);

    const auto eightBit = codec.read(eightBitPath);
    const auto sixteenBit = codec.read(sixteenBitPath);
    assert(eightBit.ok);
    assert(sixteenBit.ok);
    assert(eightBit.image.sourceBitsPerChannel == 8);
    assert(sixteenBit.image.sourceBitsPerChannel == 16);
}

void testImageCodecConvertsColorEncodingAndReportsEffectiveBitDepth() {
#ifdef __APPLE__
    const auto toLinear = [](float value) {
        return value <= 0.04045F ? value / 12.92F
                                : std::pow((value + 0.055F) / 1.055F, 2.4F);
    };
    const auto assertDisplayColor = [](const photonstack::ImageReadResult& result) {
        assert(result.ok);
        assert(result.image.colorEncoding == photonstack::ColorEncoding::SRGB);
        assert(std::fabs(result.image.pixels[0] - 0.5F) < 0.015F);
        assert(std::fabs(result.image.pixels[1] - 0.25F) < 0.015F);
        assert(std::fabs(result.image.pixels[2] - 0.75F) < 0.015F);
    };

    photonstack::ImageBuffer srgb;
    srgb.width = 1;
    srgb.height = 1;
    srgb.channels = 4;
    srgb.colorEncoding = photonstack::ColorEncoding::SRGB;
    srgb.sourceBitsPerChannel = 16;
    srgb.pixels = {0.5F, 0.25F, 0.75F, 1.0F};

    photonstack::ImageBuffer linear = srgb;
    linear.colorEncoding = photonstack::ColorEncoding::Linear;
    linear.pixels = {toLinear(0.5F), toLinear(0.25F), toLinear(0.75F), 1.0F};

    const auto directory = std::filesystem::temp_directory_path();
    const auto srgbToLinearPath = directory / "photonstack-srgb-to-linear.tiff";
    const auto linearToSrgbPath = directory / "photonstack-linear-to-srgb.tiff";
    const auto streamedPath = directory / "photonstack-srgb-to-linear-rows.tiff";
    const auto jpegPath = directory / "photonstack-effective-depth.jpg";
    const photonstack::ImageCodec codec;

    photonstack::ImageWriteOptions linearOptions;
    linearOptions.bitDepth = photonstack::ImageWriteBitDepth::Sixteen;
    linearOptions.colorSpace = photonstack::ImageWriteColorSpace::LinearSRGB;
    assert(codec.write(srgb, srgbToLinearPath, linearOptions).ok);
    assertDisplayColor(codec.read(srgbToLinearPath));

    photonstack::ImageWriteOptions srgbOptions;
    srgbOptions.bitDepth = photonstack::ImageWriteBitDepth::Sixteen;
    srgbOptions.colorSpace = photonstack::ImageWriteColorSpace::SRGB;
    assert(codec.write(linear, linearToSrgbPath, srgbOptions).ok);
    assertDisplayColor(codec.read(linearToSrgbPath));

    assert(codec.writeRows(srgb, streamedPath, [&](const photonstack::ImageRowConsumer& consumer) {
        return consumer(0, srgb.pixels.data(), srgb.pixels.size());
    }, linearOptions).ok);
    assertDisplayColor(codec.read(streamedPath));

    photonstack::ImageWriteOptions requestedJpegOptions;
    requestedJpegOptions.bitDepth = photonstack::ImageWriteBitDepth::Sixteen;
    assert(photonstack::effectiveImageWriteBitDepth(jpegPath, requestedJpegOptions) ==
           photonstack::ImageWriteBitDepth::Eight);
    assert(photonstack::effectiveImageWriteBitDepth(directory / "auto.png", {}) ==
           photonstack::ImageWriteBitDepth::Eight);
    assert(photonstack::effectiveImageWriteBitDepth(directory / "auto.tiff", {}) ==
           photonstack::ImageWriteBitDepth::Sixteen);
    assert(photonstack::effectiveImageWriteBitDepth(directory / "unsupported.bmp", {}) ==
           photonstack::ImageWriteBitDepth::Auto);
    assert(codec.write(srgb, jpegPath, requestedJpegOptions).ok);
    const photonstack::ImageInspector inspector;
    const auto jpeg = inspector.inspect(jpegPath);
    assert(jpeg.ok);
    assert(jpeg.metadata.bitsPerChannel == 8);

    std::filesystem::remove(srgbToLinearPath);
    std::filesystem::remove(linearToSrgbPath);
    std::filesystem::remove(streamedPath);
    std::filesystem::remove(jpegPath);
#endif
}

void testImageCodecRejectsUnknownOutputAndFlattensJpegTransparencyToBlack() {
#ifdef __APPLE__
    photonstack::ImageBuffer image;
    image.width = 96;
    image.height = 32;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::SRGB;
    image.pixels.assign(image.sampleCount(), 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            image.pixels[offset] = 1.0F;
            image.pixels[offset + 3] = x < 32 ? 1.0F : (x < 64 ? 0.5F : 0.0F);
        }
    }

    const auto directory = std::filesystem::temp_directory_path();
    const auto unknownPath = directory / "photonstack-unsupported-output.bmp";
    const auto jpegPath = directory / "photonstack-jpeg-alpha.jpg";
    const auto streamedJpegPath = directory / "photonstack-jpeg-alpha-streamed.jpg";
    const auto pngPath = directory / "photonstack-alpha-preserved.png";
    std::filesystem::remove(unknownPath);
    const photonstack::ImageCodec codec;
    const auto unsupported = codec.write(image, unknownPath);
    assert(!unsupported.ok);
    assert(unsupported.errorCode == "OutputFormatUnsupported");
    assert(!photonstack::isSupportedImageWritePath(unknownPath));
    assert(photonstack::isSupportedImageWritePath(jpegPath));
    assert(photonstack::isSupportedImageWritePath(directory / "supported.fits"));
    assert(!std::filesystem::exists(unknownPath));
    const auto unsupportedRows = codec.writeRows(
        image,
        unknownPath,
        [&](const photonstack::ImageRowConsumer& consumer) {
            return consumer(0, image.pixels.data(), static_cast<std::size_t>(image.width) * image.channels);
        }
    );
    assert(!unsupportedRows.ok);
    assert(unsupportedRows.errorCode == "OutputFormatUnsupported");

    assert(codec.write(image, jpegPath, {
        .bitDepth = photonstack::ImageWriteBitDepth::Eight,
        .colorSpace = photonstack::ImageWriteColorSpace::SRGB,
        .quality = 1.0F,
    }).ok);
    const auto decoded = codec.read(jpegPath);
    assert(decoded.ok);
    const auto pixel = [&](std::uint32_t x) {
        return (static_cast<std::size_t>(16) * decoded.image.width + x) * decoded.image.channels;
    };
    const auto opaque = pixel(16);
    const auto half = pixel(48);
    const auto transparent = pixel(80);
    assert(decoded.image.pixels[opaque] > 0.92F);
    assert(decoded.image.pixels[half] > 0.68F && decoded.image.pixels[half] < 0.80F);
    assert(decoded.image.pixels[transparent] < 0.08F);
    assert(decoded.image.pixels[opaque + 3] > 0.99F);
    assert(decoded.image.pixels[half + 3] > 0.99F);
    assert(decoded.image.pixels[transparent + 3] > 0.99F);

    assert(codec.write(image, pngPath, {
        .bitDepth = photonstack::ImageWriteBitDepth::Eight,
        .colorSpace = photonstack::ImageWriteColorSpace::SRGB,
        .quality = 1.0F,
    }).ok);
    const auto png = codec.read(pngPath);
    assert(png.ok);
    const auto pngHalf = (static_cast<std::size_t>(16) * png.image.width + 48) * png.image.channels;
    const auto pngTransparent = (static_cast<std::size_t>(16) * png.image.width + 80) * png.image.channels;
    assert(png.image.pixels[pngHalf] > 0.95F);
    assert(png.image.pixels[pngHalf + 3] > 0.48F && png.image.pixels[pngHalf + 3] < 0.52F);
    assert(png.image.pixels[pngTransparent + 3] < 0.01F);

    const photonstack::ImageWriteOptions jpegOptions = {
        .bitDepth = photonstack::ImageWriteBitDepth::Eight,
        .colorSpace = photonstack::ImageWriteColorSpace::SRGB,
        .quality = 1.0F,
    };
    assert(codec.writeRows(
        image,
        streamedJpegPath,
        [&](const photonstack::ImageRowConsumer& consumer) {
            const auto rowSamples = static_cast<std::size_t>(image.width) * image.channels;
            for (std::uint32_t row = 0; row < image.height; ++row) {
                if (!consumer(row, image.pixels.data() + static_cast<std::size_t>(row) * rowSamples, rowSamples)) {
                    return false;
                }
            }
            return true;
        },
        jpegOptions
    ).ok);
    const auto streamed = codec.read(streamedJpegPath);
    assert(streamed.ok);
    for (const auto x : {16U, 48U, 80U}) {
        const auto fullOffset = pixel(x);
        const auto streamedOffset =
            (static_cast<std::size_t>(16) * streamed.image.width + x) * streamed.image.channels;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            assert(std::fabs(decoded.image.pixels[fullOffset + channel] -
                             streamed.image.pixels[streamedOffset + channel]) < 0.02F);
        }
    }
    std::filesystem::remove(jpegPath);
    std::filesystem::remove(streamedJpegPath);
    std::filesystem::remove(pngPath);
#endif
}

void testRasterWritersSanitizeNonFiniteSamples() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::Linear;
    image.pixels = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        1.0F,
        0.5F, 0.25F, 0.75F,
        std::numeric_limits<float>::quiet_NaN(),
    };

    const photonstack::ImageCodec codec;
    const auto fullPath = std::filesystem::temp_directory_path() / "photonstack-nonfinite-full.png";
    assert(codec.write(image, fullPath).ok);
    const auto full = codec.read(fullPath);
    assert(full.ok);
    assert(full.image.pixels[0] < 0.001F);
    assert(full.image.pixels[1] < 0.001F);
    assert(full.image.pixels[2] < 0.001F);
    assert(full.image.pixels[3] > 0.999F);
    assert(full.image.pixels[4] < 0.001F);
    assert(full.image.pixels[5] < 0.001F);
    assert(full.image.pixels[6] < 0.001F);
    assert(full.image.pixels[7] < 0.001F);

    const auto streamedPath = std::filesystem::temp_directory_path() / "photonstack-nonfinite-streamed.tiff";
    const auto streamedWrite = codec.writeRows(
        image,
        streamedPath,
        [&](const photonstack::ImageRowConsumer& consumer) {
            return consumer(0, image.pixels.data(), image.pixels.size());
        },
        {.bitDepth = photonstack::ImageWriteBitDepth::Sixteen}
    );
    assert(streamedWrite.ok);
    const auto streamed = codec.read(streamedPath);
    assert(streamed.ok);
    for (std::size_t sample = 0; sample < streamed.image.pixels.size(); ++sample) {
        assert(std::isfinite(streamed.image.pixels[sample]));
    }
    assert(streamed.image.pixels[0] < 0.001F);
    assert(streamed.image.pixels[3] > 0.999F);
    assert(streamed.image.pixels[7] < 0.001F);
}

void testImageCodecAppliesMetadataOrientation() {
#ifdef __APPLE__
    photonstack::ImageBuffer image;
    image.width = 4;
    image.height = 2;
    image.channels = 4;
    image.pixels.resize(image.sampleCount(), 1.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * image.channels;
            const bool red = x < 2;
            image.pixels[offset] = red ? 1.0F : 0.0F;
            image.pixels[offset + 1] = red ? 0.0F : 1.0F;
            image.pixels[offset + 2] = 0.0F;
            image.pixels[offset + 3] = 1.0F;
        }
    }

    const photonstack::ImageCodec codec;
    const auto sourcePath = std::filesystem::temp_directory_path() / "photonstack-orientation-source.tiff";
    assert(codec.write(image, sourcePath, {.bitDepth = photonstack::ImageWriteBitDepth::Sixteen}).ok);
    const auto orientedPath =
        copyImageWithOrientation(sourcePath, "photonstack-orientation-right.tiff", CFSTR("public.tiff"), 6);

    const photonstack::ImageInspector inspector;
    const auto metadata = inspector.inspect(orientedPath);
    assert(metadata.ok);
    assert(metadata.metadata.orientation == 6);
    assert(metadata.metadata.width == 2);
    assert(metadata.metadata.height == 4);
    assert(metadata.metadata.bitsPerChannel == 16);
    assert(metadata.metadata.whiteBalance.empty());

    const auto decoded = codec.read(orientedPath);
    assert(decoded.ok);
    assert(decoded.image.width == 2);
    assert(decoded.image.height == 4);
    assert(decoded.image.sourceBitsPerChannel == 16);
    const auto firstRow = decoded.image.pixels.data();
    const auto lastRow = decoded.image.pixels.data() +
                         static_cast<std::size_t>(decoded.image.height - 1) * decoded.image.width * decoded.image.channels;
    assert(firstRow[0] > 0.95F);
    assert(firstRow[1] < 0.05F);
    assert(lastRow[0] < 0.05F);
    assert(lastRow[1] > 0.95F);

    const auto normalizedPath = std::filesystem::temp_directory_path() / "photonstack-orientation-normalized.tiff";
    assert(codec.write(decoded.image, normalizedPath, {.bitDepth = photonstack::ImageWriteBitDepth::Sixteen}).ok);
    const auto normalizedMetadata = inspector.inspect(normalizedPath);
    assert(normalizedMetadata.ok);
    assert(normalizedMetadata.metadata.width == 2);
    assert(normalizedMetadata.metadata.height == 4);
    assert(normalizedMetadata.metadata.orientation <= 1);

    const auto jpegPath =
        copyImageWithOrientation(sourcePath, "photonstack-orientation-right.jpg", CFSTR("public.jpeg"), 6);
    const auto jpegMetadata = inspector.inspect(jpegPath);
    const auto jpeg = codec.read(jpegPath);
    assert(jpegMetadata.ok);
    assert(jpeg.ok);
    assert(jpegMetadata.metadata.format == photonstack::ImageFormat::JPEG);
    assert(jpegMetadata.metadata.orientation == 6);
    assert(jpegMetadata.metadata.width == 2);
    assert(jpegMetadata.metadata.height == 4);
    assert(jpegMetadata.metadata.bitsPerChannel == 8);
    assert(jpeg.image.width == 2);
    assert(jpeg.image.height == 4);
    assert(jpeg.image.sourceBitsPerChannel == 8);

    for (const std::int32_t orientation : {2, 3, 4, 5, 7, 8}) {
        const auto variantPath = copyImageWithOrientation(
            sourcePath,
            "photonstack-orientation-" + std::to_string(orientation) + ".tiff",
            CFSTR("public.tiff"),
            orientation
        );
        const auto variantMetadata = inspector.inspect(variantPath);
        const auto variant = codec.read(variantPath);
        assert(variantMetadata.ok);
        assert(variant.ok);
        assert(variantMetadata.metadata.orientation == static_cast<std::uint32_t>(orientation));
        const bool swapsDimensions = orientation >= 5;
        assert(variantMetadata.metadata.width == (swapsDimensions ? 2 : 4));
        assert(variantMetadata.metadata.height == (swapsDimensions ? 4 : 2));
        assert(variant.image.width == variantMetadata.metadata.width);
        assert(variant.image.height == variantMetadata.metadata.height);
        assert(variant.image.sourceBitsPerChannel == 16);
        assert(variant.backend == photonstack::ImageReadBackend::ImageIO);
        assert(!variant.usedFallback);
        assert(variant.fallbackErrorCode.empty());
        assert(variant.fallbackMessage.empty());
    }
#endif
}

void testFitsInspectReadWrite() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.0F, 0.25F, 0.5F, 1.0F, 1.0F, 0.75F, 0.25F, 1.0F,
    };

    const auto path = std::filesystem::temp_directory_path() / "photonstack-write.fits";

    const photonstack::ImageCodec codec;
    assert(codec.write(image, path).ok);

    const photonstack::ImageInspector inspector;
    const auto metadata = inspector.inspect(path);
    assert(metadata.ok);
    assert(metadata.metadata.format == photonstack::ImageFormat::FITS);
    assert(metadata.metadata.width == 2);
    assert(metadata.metadata.height == 1);
    assert(metadata.metadata.channels == 4);
    assert(metadata.metadata.bitsPerChannel == 32);
    assert(metadata.metadata.colorModel == "RGB");
    assert(metadata.metadata.colorProfile == "LINEAR-SRGB");

    const auto read = codec.read(path);
    assert(read.ok);
    assert(read.backend == photonstack::ImageReadBackend::Fits);
    assert(!read.usedFallback);
    assert(read.fallbackErrorCode.empty());
    assert(read.fallbackMessage.empty());
    assert(std::string(photonstack::imageReadBackendName(read.backend)) == "fits");
    assert(std::string(photonstack::imageReadBackendName(photonstack::ImageReadBackend::Unknown)) == "unknown");
    assert(std::string(photonstack::imageReadBackendName(photonstack::ImageReadBackend::ImageIO)) == "imageio");
    assert(std::string(photonstack::imageReadBackendName(photonstack::ImageReadBackend::AppleRaw)) == "apple-raw");
    assert(std::string(photonstack::imageReadBackendName(static_cast<photonstack::ImageReadBackend>(999))) ==
           "unknown");
    assert(read.image.width == 2);
    assert(read.image.height == 1);
    assert(read.image.channels == 4);
    assert(std::fabs(read.image.pixels[0] - 0.0F) < 0.001F);
    assert(std::fabs(read.image.pixels[1] - 0.25F) < 0.001F);
    assert(std::fabs(read.image.pixels[4] - 1.0F) < 0.001F);
}

void testFitsWriteConvertsSRGBToLinearAndPreservesAlpha() {
    const auto toLinear = [](float value) {
        return value <= 0.04045F ? value / 12.92F
                                : std::pow((value + 0.055F) / 1.055F, 2.4F);
    };
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::SRGB;
    image.pixels = {
        0.5F, 0.25F, 0.75F, 0.5F,
        0.1F, 0.9F, 0.4F, 0.25F,
    };

    const auto path = std::filesystem::temp_directory_path() / "photonstack-srgb-linearized.fits";
    const photonstack::ImageCodec codec;
    assert(codec.write(image, path).ok);
    const auto bytes = readBytes(path);
    const std::string header(bytes.begin(), bytes.begin() + 2880);
    assert(header.find("PSCOLOR") != std::string::npos);
    assert(header.find("'LINEAR-SRGB'") != std::string::npos);

    const auto decoded = codec.read(path);
    assert(decoded.ok);
    assert(decoded.image.colorEncoding == photonstack::ColorEncoding::Linear);
    assert(decoded.image.sourceBitsPerChannel == 32);
    for (std::size_t pixel = 0; pixel < image.pixelCount(); ++pixel) {
        const auto offset = pixel * image.channels;
        assert(std::fabs(decoded.image.pixels[offset] - toLinear(image.pixels[offset])) < 1.0e-6F);
        assert(std::fabs(decoded.image.pixels[offset + 1] - toLinear(image.pixels[offset + 1])) < 1.0e-6F);
        assert(std::fabs(decoded.image.pixels[offset + 2] - toLinear(image.pixels[offset + 2])) < 1.0e-6F);
        assert(std::fabs(decoded.image.pixels[offset + 3] - image.pixels[offset + 3]) < 1.0e-6F);
    }

    photonstack::ImageBuffer gray;
    gray.width = 1;
    gray.height = 1;
    gray.channels = 1;
    gray.format = photonstack::PixelFormat::Float32Gray;
    gray.colorEncoding = photonstack::ColorEncoding::SRGB;
    gray.pixels = {0.5F};
    const auto grayPath = std::filesystem::temp_directory_path() / "photonstack-srgb-gray-linearized.fits";
    assert(codec.write(gray, grayPath).ok);
    const auto decodedGray = codec.read(grayPath);
    assert(decodedGray.ok);
    assert(std::fabs(decodedGray.image.pixels[0] - toLinear(0.5F)) < 1.0e-6F);
    assert(std::fabs(decodedGray.image.pixels[1] - toLinear(0.5F)) < 1.0e-6F);
    assert(std::fabs(decodedGray.image.pixels[2] - toLinear(0.5F)) < 1.0e-6F);
    assert(decodedGray.image.pixels[3] == 1.0F);
}

void testFitsScientificReadPreservesPhysicalValues() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::Linear;
    image.pixels = {
        -2.0F, 0.5F, 3.0F, 0.25F,
        4.0F, -1.0F, 2.0F, 0.75F,
    };

    const auto path = std::filesystem::temp_directory_path() / "photonstack-scientific-values.fits";
    const photonstack::ImageCodec codec;
    assert(codec.write(image, path).ok);

    photonstack::ImageReadOptions scientificOptions;
    scientificOptions.fits.mode = photonstack::FitsDecodeMode::Scientific;
    const auto scientific = codec.read(path, scientificOptions);
    assert(scientific.ok);
    assert(scientific.image.colorEncoding == photonstack::ColorEncoding::Linear);
    for (std::size_t sample = 0; sample < image.pixels.size(); ++sample) {
        assert(std::fabs(scientific.image.pixels[sample] - image.pixels[sample]) < 1.0e-6F);
    }

    const auto display = codec.read(path);
    assert(display.ok);
    assert(std::fabs(display.image.pixels[0] - 0.0F) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[1] - (2.5F / 6.0F)) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[2] - (5.0F / 6.0F)) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[3] - 0.25F) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[4] - 1.0F) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[5] - (1.0F / 6.0F)) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[6] - (4.0F / 6.0F)) < 1.0e-6F);
    assert(std::fabs(display.image.pixels[7] - 0.75F) < 1.0e-6F);

    photonstack::ImageBuffer scaledSource;
    scaledSource.width = 2;
    scaledSource.height = 1;
    scaledSource.channels = 1;
    scaledSource.format = photonstack::PixelFormat::Float32Gray;
    scaledSource.colorEncoding = photonstack::ColorEncoding::Linear;
    scaledSource.pixels = {-2.0F, 3.0F};
    const auto scaledPath = std::filesystem::temp_directory_path() / "photonstack-scaled-values.fits";
    assert(codec.write(scaledSource, scaledPath).ok);
    auto scaledBytes = readBytes(scaledPath);
    overwriteFitsCardValue(scaledBytes, "BSCALE", "2.0");
    overwriteFitsCardValue(scaledBytes, "BZERO", "10.0");
    writeBytes("photonstack-scaled-values.fits", scaledBytes);

    const auto scaled = codec.read(scaledPath, scientificOptions);
    assert(scaled.ok);
    assert(std::fabs(scaled.image.pixels[0] - 6.0F) < 1.0e-6F);
    assert(std::fabs(scaled.image.pixels[4] - 16.0F) < 1.0e-6F);

    photonstack::FitsDecodeOptions invalidOptions;
    invalidOptions.mode = static_cast<photonstack::FitsDecodeMode>(999);
    const auto invalid = photonstack::FitsCodec().read(path, invalidOptions);
    assert(!invalid.ok);
    assert(invalid.errorCode == "ArgumentInvalid");
}

void testFitsScientificValuesFlowThroughStackAndDrizzle() {
    photonstack::ImageBuffer first;
    first.width = 2;
    first.height = 1;
    first.channels = 4;
    first.colorEncoding = photonstack::ColorEncoding::Linear;
    first.pixels = {
        -2.0F, 4.0F, 8.0F, 1.0F,
        -2.0F, 4.0F, 8.0F, 1.0F,
    };
    photonstack::ImageBuffer second = first;
    second.pixels = {
        2.0F, 6.0F, 10.0F, 1.0F,
        2.0F, 6.0F, 10.0F, 1.0F,
    };

    const auto firstPath = std::filesystem::temp_directory_path() / "photonstack-scientific-stack-a.fits";
    const auto secondPath = std::filesystem::temp_directory_path() / "photonstack-scientific-stack-b.fits";
    const photonstack::ImageCodec codec;
    assert(codec.write(first, firstPath).ok);
    assert(codec.write(second, secondPath).ok);

    const auto stacked = photonstack::Stacker().average({firstPath, secondPath});
    assert(stacked.ok);
    assert(std::fabs(stacked.image.pixels[0] - 0.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[1] - 5.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[2] - 9.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[3] - 1.0F) < 1.0e-6F);

    photonstack::DrizzleOptions drizzleOptions;
    drizzleOptions.scale = 1;
    drizzleOptions.alignment = photonstack::DrizzleAlignment::None;
    const auto drizzled = photonstack::DrizzleStacker().drizzle({firstPath, secondPath}, drizzleOptions);
    assert(drizzled.ok);
    assert(std::fabs(drizzled.image.pixels[0] - 0.0F) < 1.0e-6F);
    assert(std::fabs(drizzled.image.pixels[1] - 5.0F) < 1.0e-6F);
    assert(std::fabs(drizzled.image.pixels[2] - 9.0F) < 1.0e-6F);
    assert(std::fabs(drizzled.image.pixels[3] - 1.0F) < 1.0e-6F);

    photonstack::MosaicOptions mosaicOptions;
    mosaicOptions.overlapPixels = 1;
    mosaicOptions.blendMode = photonstack::MosaicBlendMode::Average;
    mosaicOptions.exposureMatching = false;
    const auto mosaic = photonstack::MosaicBuilder().stitchHorizontal({firstPath, secondPath}, mosaicOptions);
    assert(mosaic.ok);
    assert(mosaic.image.width == 3);
    assert(mosaic.image.height == 1);
    constexpr std::size_t overlapOffset = 4;
    assert(std::fabs(mosaic.image.pixels[overlapOffset] - 0.0F) < 1.0e-6F);
    assert(std::fabs(mosaic.image.pixels[overlapOffset + 1] - 5.0F) < 1.0e-6F);
    assert(std::fabs(mosaic.image.pixels[overlapOffset + 2] - 9.0F) < 1.0e-6F);
    assert(std::fabs(mosaic.image.pixels[overlapOffset + 3] - 1.0F) < 1.0e-6F);
}

void testFitsNonFinitePixelsCanBePreservedOrMasked() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.colorEncoding = photonstack::ColorEncoding::Linear;
    image.pixels = {
        std::numeric_limits<float>::quiet_NaN(), 2.0F, 3.0F, 1.0F,
        4.0F, 5.0F, 6.0F, 1.0F,
    };
    const auto invalidPath = std::filesystem::temp_directory_path() / "photonstack-nonfinite.fits";
    const photonstack::ImageCodec codec;
    assert(codec.write(image, invalidPath).ok);

    photonstack::ImageReadOptions preserveOptions;
    preserveOptions.fits.mode = photonstack::FitsDecodeMode::Scientific;
    const auto preserved = codec.read(invalidPath, preserveOptions);
    assert(preserved.ok);
    assert(std::isnan(preserved.image.pixels[0]));
    assert(preserved.image.pixels[3] == 1.0F);

    auto maskOptions = preserveOptions;
    maskOptions.fits.maskNonFinitePixels = true;
    const auto masked = codec.read(invalidPath, maskOptions);
    assert(masked.ok);
    assert(masked.image.pixels[0] == 0.0F);
    assert(masked.image.pixels[1] == 0.0F);
    assert(masked.image.pixels[2] == 0.0F);
    assert(masked.image.pixels[3] == 0.0F);
    assert(masked.image.pixels[4] == 4.0F);
    assert(masked.image.pixels[7] == 1.0F);

    photonstack::ImageBuffer fallback = image;
    fallback.pixels = {
        10.0F, 20.0F, 30.0F, 1.0F,
        8.0F, 9.0F, 10.0F, 1.0F,
    };
    const auto fallbackPath = std::filesystem::temp_directory_path() / "photonstack-nonfinite-fallback.fits";
    assert(codec.write(fallback, fallbackPath).ok);
    const auto stacked = photonstack::Stacker().average({invalidPath, fallbackPath});
    assert(stacked.ok);
    assert(std::fabs(stacked.image.pixels[0] - 10.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[1] - 20.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[2] - 30.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[3] - 0.5F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[4] - 6.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[5] - 7.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[6] - 8.0F) < 1.0e-6F);
    assert(std::fabs(stacked.image.pixels[7] - 1.0F) < 1.0e-6F);
}

void testFitsRejectsMalformedNumericHeaders() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.5F);

    const photonstack::ImageCodec codec;
    const photonstack::ImageInspector inspector;
    const auto validPath = std::filesystem::temp_directory_path() / "photonstack-valid-numeric-header.fits";
    assert(codec.write(image, validPath).ok);
    const auto validBytes = readBytes(validPath);

    auto malformedScale = validBytes;
    overwriteFitsCardValue(malformedScale, "BSCALE", "NaN");
    const auto malformedScalePath = writeBytes("photonstack-malformed-scale.fits", malformedScale);
    const auto scaleRead = codec.read(malformedScalePath);
    assert(!scaleRead.ok);
    assert(scaleRead.errorCode == "ImageHeaderInvalid");

    auto partialWidth = validBytes;
    overwriteFitsCardValue(partialWidth, "NAXIS1", "2junk");
    const auto partialWidthPath = writeBytes("photonstack-partial-width.fits", partialWidth);
    assert(!inspector.inspect(partialWidthPath).ok);
    assert(!codec.read(partialWidthPath).ok);

    auto unsupportedAxisCount = validBytes;
    overwriteFitsCardValue(unsupportedAxisCount, "NAXIS", "4");
    const auto unsupportedAxisPath = writeBytes("photonstack-unsupported-axis-count.fits", unsupportedAxisCount);
    assert(!inspector.inspect(unsupportedAxisPath).ok);
    assert(!codec.read(unsupportedAxisPath).ok);

    auto overflowingWidth = validBytes;
    overwriteFitsCardValue(overflowingWidth, "NAXIS1", "4294967296");
    const auto overflowingWidthPath = writeBytes("photonstack-overflowing-width.fits", overflowingWidth);
    assert(!inspector.inspect(overflowingWidthPath).ok);
    assert(!codec.read(overflowingWidthPath).ok);

    photonstack::ImageBuffer unsupportedChannels;
    unsupportedChannels.width = 1;
    unsupportedChannels.height = 1;
    unsupportedChannels.channels = 5;
    unsupportedChannels.pixels.assign(unsupportedChannels.sampleCount(), 0.5F);
    const auto unsupportedChannelsPath =
        std::filesystem::temp_directory_path() / "photonstack-unsupported-channels.fits";
    const auto unsupportedWrite = codec.write(unsupportedChannels, unsupportedChannelsPath);
    assert(!unsupportedWrite.ok);
    assert(unsupportedWrite.errorCode == "ImageBufferInvalid");
}

void testFitsStreamingHeaderAndPayloadIO() {
    photonstack::ImageBuffer image;
    image.width = 2;
    image.height = 1;
    image.channels = 4;
    image.pixels = {
        0.1F, 0.2F, 0.3F, 1.0F,
        0.7F, 0.8F, 0.9F, 0.5F,
    };

    const photonstack::ImageCodec codec;
    const photonstack::ImageInspector inspector;
    const auto validPath = std::filesystem::temp_directory_path() / "photonstack-streaming-valid.fits";
    assert(codec.write(image, validPath).ok);
    assert(std::filesystem::file_size(validPath) == 2 * 2880);

    auto multiBlockBytes = readBytes(validPath);
    assert(multiBlockBytes.size() == 2 * 2880);
    constexpr std::size_t endCardOffset = 9 * 80;
    std::fill_n(multiBlockBytes.begin() + static_cast<std::ptrdiff_t>(endCardOffset), 80, ' ');
    multiBlockBytes.insert(multiBlockBytes.begin() + 2880, 2880, ' ');
    multiBlockBytes[2880] = 'E';
    multiBlockBytes[2881] = 'N';
    multiBlockBytes[2882] = 'D';
    const auto multiBlockPath = writeBytes("photonstack-streaming-multiblock.fits", multiBlockBytes);
    const auto multiBlockMetadata = inspector.inspect(multiBlockPath);
    const auto multiBlockRead = codec.read(multiBlockPath);
    assert(multiBlockMetadata.ok);
    assert(multiBlockRead.ok);
    assert(multiBlockRead.image.width == image.width);
    assert(std::fabs(multiBlockRead.image.pixels[0] - image.pixels[0]) < 1.0e-6F);
    assert(std::fabs(multiBlockRead.image.pixels[7] - image.pixels[7]) < 1.0e-6F);

    const auto sparsePath = std::filesystem::temp_directory_path() / "photonstack-streaming-large-tail.fits";
    std::error_code error;
    std::filesystem::copy_file(validPath, sparsePath, std::filesystem::copy_options::overwrite_existing, error);
    assert(!error);
    constexpr std::uintmax_t sparseSize = 64ULL * 1024ULL * 1024ULL;
    std::filesystem::resize_file(sparsePath, sparseSize, error);
    assert(!error);
    assert(std::filesystem::file_size(sparsePath) == sparseSize);
    assert(inspector.inspect(sparsePath).ok);
    const auto sparseRead = codec.read(sparsePath);
    assert(sparseRead.ok);
    assert(std::fabs(sparseRead.image.pixels[4] - image.pixels[4]) < 1.0e-6F);

    const auto truncatedPath = std::filesystem::temp_directory_path() / "photonstack-streaming-truncated.fits";
    std::filesystem::copy_file(validPath, truncatedPath, std::filesystem::copy_options::overwrite_existing, error);
    assert(!error);
    std::filesystem::resize_file(truncatedPath, 2880 + image.sampleCount() * sizeof(float) - 1, error);
    assert(!error);
    assert(inspector.inspect(truncatedPath).ok);
    const auto truncatedRead = codec.read(truncatedPath);
    assert(!truncatedRead.ok);
    assert(truncatedRead.errorCode == "InputReadFailed");

    std::filesystem::remove(sparsePath, error);
    std::filesystem::remove(truncatedPath, error);
    std::filesystem::remove(multiBlockPath, error);
}

void testTileProcessor() {
    photonstack::ImageBuffer image;
    image.width = 5;
    image.height = 3;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.0F);

    const photonstack::TileProcessor processor;
    std::size_t coveredPixels = 0;
    const auto result =
        processor.forEachTile(image, {.tileWidth = 2, .tileHeight = 2}, [&](const photonstack::TileRect& tile) {
            coveredPixels += static_cast<std::size_t>(tile.width) * tile.height;
        });

    assert(result.ok);
    assert(result.tiles == 6);
    assert(coveredPixels == image.pixelCount());

    const auto budgeted = processor.optionsForBudget(
        image,
        {.maxBytes = 4 * 4 * sizeof(float), .preferredTileWidth = 8, .preferredTileHeight = 8}
    );
    assert(budgeted.tileWidth <= 4);
    assert(budgeted.tileHeight <= 4);
    assert(static_cast<std::size_t>(budgeted.tileWidth) * budgeted.tileHeight * image.channels * sizeof(float) <=
           4 * 4 * sizeof(float));

    photonstack::ImageBuffer wide;
    wide.width = 2000;
    wide.height = 1000;
    wide.channels = 4;
    const auto wideBudgeted = processor.optionsForBudget(
        wide,
        {.maxBytes = 512U * 128U * 4U * sizeof(float),
         .preferredTileWidth = 1024, .preferredTileHeight = 256}
    );
    assert(wideBudgeted.tileWidth == 512);
    assert(wideBudgeted.tileHeight == 128);
    assert(static_cast<std::size_t>(wideBudgeted.tileWidth) * wideBudgeted.tileHeight *
               wide.channels * sizeof(float) <=
           512U * 128U * 4U * sizeof(float));

    const auto minimal = processor.optionsForBudget(
        wide,
        {.maxBytes = 0, .preferredTileWidth = std::numeric_limits<std::uint32_t>::max(),
         .preferredTileHeight = 1}
    );
    assert(minimal.tileWidth == 1);
    assert(minimal.tileHeight == 1);
}

void testProcessingOptionsRejectNonFiniteValues() {
    photonstack::ImageBuffer image;
    image.width = 16;
    image.height = 16;
    image.channels = 4;
    image.pixels.assign(image.sampleCount(), 0.2F);
    image.pixels[(static_cast<std::size_t>(8) * image.width + 8) * image.channels] = 0.9F;
    image.pixels[(static_cast<std::size_t>(8) * image.width + 8) * image.channels + 1] = 0.9F;
    image.pixels[(static_cast<std::size_t>(8) * image.width + 8) * image.channels + 2] = 0.9F;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const auto expectArgumentInvalid = [](const auto& result) {
        assert(!result.ok);
        assert(result.errorCode == "ArgumentInvalid");
    };

    photonstack::NoiseReductionOptions noiseOptions;
    noiseOptions.amount = nan;
    expectArgumentInvalid(photonstack::NoiseReducer().reduce(image, noiseOptions));
    noiseOptions = {};
    noiseOptions.radius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::NoiseReducer().reduce(image, noiseOptions));

    photonstack::SharpenOptions sharpenOptions;
    sharpenOptions.threshold = infinity;
    expectArgumentInvalid(photonstack::Sharpen().unsharpMask(image, sharpenOptions));
    sharpenOptions = {};
    sharpenOptions.radius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::Sharpen().unsharpMask(image, sharpenOptions));

    photonstack::LocalContrastOptions contrastOptions;
    contrastOptions.amount = nan;
    expectArgumentInvalid(photonstack::LocalContrast().apply(image, contrastOptions));
    contrastOptions = {};
    contrastOptions.radius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::LocalContrast().apply(image, contrastOptions));

    photonstack::DeconvolutionOptions deconvolutionOptions;
    deconvolutionOptions.sigma = nan;
    expectArgumentInvalid(photonstack::Deconvolution().richardsonLucy(image, deconvolutionOptions));
    deconvolutionOptions = {};
    deconvolutionOptions.radius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::Deconvolution().richardsonLucy(image, deconvolutionOptions));
    deconvolutionOptions = {};
    deconvolutionOptions.damping = 1.01F;
    expectArgumentInvalid(photonstack::Deconvolution().richardsonLucy(image, deconvolutionOptions));

    photonstack::BackgroundNeutralizationOptions neutralizationOptions;
    neutralizationOptions.epsilon = nan;
    expectArgumentInvalid(photonstack::ColorAdjuster().neutralizeBackground(image, neutralizationOptions));
    photonstack::SaturationOptions saturationOptions;
    saturationOptions.amount = infinity;
    expectArgumentInvalid(photonstack::ColorAdjuster().adjustSaturation(image, saturationOptions));
    photonstack::GreenCastSuppressionOptions greenOptions;
    greenOptions.backgroundLimit = nan;
    expectArgumentInvalid(photonstack::ColorAdjuster().suppressGreenCast(image, greenOptions));

    photonstack::CurvesOptions curvesOptions;
    curvesOptions.points[1].output = nan;
    expectArgumentInvalid(photonstack::Curves().apply(image, curvesOptions));
    curvesOptions = {};
    curvesOptions.channel = static_cast<photonstack::CurveChannel>(999);
    expectArgumentInvalid(photonstack::Curves().apply(image, curvesOptions));

    photonstack::StretchOptions stretchOptions;
    stretchOptions.blackPoint = nan;
    expectArgumentInvalid(photonstack::Stretch().apply(image, stretchOptions));
    photonstack::AutoStretchOptions autoStretchOptions;
    autoStretchOptions.arcsinhStrength = infinity;
    expectArgumentInvalid(photonstack::Stretch().applyAuto(image, autoStretchOptions));
    const auto estimated = photonstack::Stretch().estimateAuto(image, autoStretchOptions);
    assert(std::isfinite(estimated.blackPoint));
    assert(std::isfinite(estimated.midPoint));
    assert(std::isfinite(estimated.whitePoint));
    assert(std::isfinite(estimated.arcsinhStrength));

    photonstack::BackgroundExtractionOptions backgroundOptions;
    backgroundOptions.strength = nan;
    expectArgumentInvalid(photonstack::BackgroundExtractor().extractGlobal(image, backgroundOptions));
    backgroundOptions = {};
    backgroundOptions.mode = static_cast<photonstack::BackgroundMode>(999);
    expectArgumentInvalid(photonstack::BackgroundExtractor().extractGlobal(image, backgroundOptions));
    photonstack::BackgroundGridOptions backgroundGridOptions;
    backgroundGridOptions.extraction.epsilon = infinity;
    expectArgumentInvalid(photonstack::BackgroundExtractor().extractGrid(image, backgroundGridOptions));
    backgroundGridOptions = {};
    backgroundGridOptions.columns = image.width + 1;
    expectArgumentInvalid(photonstack::BackgroundExtractor().extractGrid(image, backgroundGridOptions));

    photonstack::FrameNormalizationOptions normalizationOptions;
    normalizationOptions.targetScale = nan;
    expectArgumentInvalid(photonstack::FrameNormalizer().normalize(image, normalizationOptions));
    expectArgumentInvalid(photonstack::FrameNormalizer().matchReference(image, image, normalizationOptions));
    normalizationOptions = {};
    normalizationOptions.targetBackground = -0.1F;
    expectArgumentInvalid(photonstack::FrameNormalizer().normalize(image, normalizationOptions));
    normalizationOptions.targetBackground = 1.1F;
    expectArgumentInvalid(photonstack::FrameNormalizer().matchReference(image, image, normalizationOptions));
    normalizationOptions = {};
    normalizationOptions.targetScale = -0.1F;
    expectArgumentInvalid(photonstack::FrameNormalizer().normalize(image, normalizationOptions));
    photonstack::LocalNormalizationOptions localNormalizationOptions;
    localNormalizationOptions.normalization.targetBackground = infinity;
    expectArgumentInvalid(
        photonstack::FrameNormalizer().matchReferenceLocal(image, image, localNormalizationOptions)
    );
    localNormalizationOptions = {};
    localNormalizationOptions.rows = image.height + 1;
    expectArgumentInvalid(
        photonstack::FrameNormalizer().matchReferenceLocal(image, image, localNormalizationOptions)
    );

    photonstack::StarDetectionOptions detectionOptions;
    detectionOptions.minPeak = nan;
    expectArgumentInvalid(photonstack::StarDetector().detect(image, detectionOptions));
    const std::vector<float> luminance(image.pixelCount(), 0.2F);
    expectArgumentInvalid(
        photonstack::StarDetector().detectLuminance(image.width, image.height, luminance, detectionOptions)
    );

    photonstack::StarMaskOptions maskOptions;
    maskOptions.opacity = nan;
    expectArgumentInvalid(photonstack::StarMask().create(image, maskOptions));
    maskOptions = {};
    maskOptions.opacity = 1.1F;
    expectArgumentInvalid(photonstack::StarMask().create(image, maskOptions));
    maskOptions = {};
    maskOptions.largeRadius = std::numeric_limits<std::uint32_t>::max();
    maskOptions.layered = true;
    expectArgumentInvalid(photonstack::StarMask().create(image, maskOptions));
    photonstack::StarReductionOptions reductionOptions;
    reductionOptions.haloProtection = infinity;
    expectArgumentInvalid(photonstack::StarReducer().reduce(image, reductionOptions));
    reductionOptions = {};
    reductionOptions.radius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::StarReducer().reduce(image, reductionOptions));
    photonstack::ComaReductionOptions comaOptions;
    comaOptions.eccentricityThreshold = nan;
    expectArgumentInvalid(photonstack::ComaReducer().reduce(image, comaOptions));
    comaOptions = {};
    comaOptions.radius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::ComaReducer().reduce(image, comaOptions));

    photonstack::FrameQualityOptions qualityOptions;
    qualityOptions.saturationThreshold = nan;
    expectArgumentInvalid(photonstack::FrameQualityAnalyzer().analyze(image, qualityOptions));
    expectArgumentInvalid(
        photonstack::FrameQualityAnalyzer().analyzeLuminance(image.width, image.height, luminance, qualityOptions)
    );

    photonstack::CalibrationOptions calibrationOptions;
    calibrationOptions.flatEpsilon = nan;
    expectArgumentInvalid(photonstack::Calibrator().calibrate(image, calibrationOptions));
    calibrationOptions.flatEpsilon = 1.0F;
    expectArgumentInvalid(photonstack::Calibrator().calibrate(image, calibrationOptions));

    photonstack::ArtifactTrailOptions artifactOptions;
    artifactOptions.airplaneLength = nan;
    expectArgumentInvalid(photonstack::ArtifactTrailRemover().detect(image, artifactOptions));
    artifactOptions = {};
    artifactOptions.maskRadius = std::numeric_limits<std::uint32_t>::max();
    expectArgumentInvalid(photonstack::ArtifactTrailRemover().detect(image, artifactOptions));
    artifactOptions = {};
    artifactOptions.selectedIndices = {std::numeric_limits<std::size_t>::max()};
    expectArgumentInvalid(photonstack::ArtifactTrailRemover().remove(image, artifactOptions));

    photonstack::CloudRemovalOptions cloudOptions;
    cloudOptions.minCoverage = nan;
    expectArgumentInvalid(photonstack::CloudRemoval().detect(image, cloudOptions));
    expectArgumentInvalid(photonstack::CloudRemoval().remove(image, cloudOptions));
    cloudOptions = {};
    cloudOptions.columns = 0;
    expectArgumentInvalid(photonstack::CloudRemoval().detect(image, cloudOptions));
    cloudOptions = {};
    cloudOptions.rows = 65;
    expectArgumentInvalid(photonstack::CloudRemoval().remove(image, cloudOptions));
    cloudOptions = {};
    cloudOptions.selectedIndices = {std::numeric_limits<std::size_t>::max()};
    expectArgumentInvalid(photonstack::CloudRemoval().remove(image, cloudOptions));

    photonstack::ImageBuffer tinyCloudImage;
    tinyCloudImage.width = 2;
    tinyCloudImage.height = 2;
    tinyCloudImage.channels = 4;
    tinyCloudImage.pixels.assign(tinyCloudImage.sampleCount(), 0.1F);
    const auto tinyCloudResult = photonstack::CloudRemoval().detect(tinyCloudImage);
    assert(tinyCloudResult.ok);

    photonstack::MeteorLayerOptions meteorOptions;
    meteorOptions.maskRadius = infinity;
    expectArgumentInvalid(photonstack::MeteorLayerComposer().extract(image, meteorOptions));
    meteorOptions = {};
    meteorOptions.maskRadius = 65.0F;
    expectArgumentInvalid(photonstack::MeteorLayerComposer().extract(image, meteorOptions));
    meteorOptions = {};
    meteorOptions.featherRadius = 65.0F;
    expectArgumentInvalid(photonstack::MeteorLayerComposer().extract(image, meteorOptions));
    meteorOptions = {};
    meteorOptions.opacity = nan;
    expectArgumentInvalid(photonstack::MeteorLayerComposer().compose(image, image, meteorOptions));

    photonstack::RegistrationOptions registrationOptions;
    registrationOptions.matchTolerance = nan;
    expectArgumentInvalid(photonstack::Registration().estimateTranslation(image, image, registrationOptions));
    expectArgumentInvalid(photonstack::Registration().estimateSimilarity(image, image, registrationOptions));
    expectArgumentInvalid(photonstack::Registration().estimateAffine(image, image, registrationOptions));
    photonstack::DistortionTransform estimatedTransform;
    expectArgumentInvalid(
        photonstack::Registration().estimateDistortion(image, image, estimatedTransform, registrationOptions)
    );

    const photonstack::Registration registration;
    const auto rowConsumer = [](std::uint32_t, const float*, std::size_t) { return true; };
    assert(!registration.renderTranslationRows(image, {.dx = nan}, rowConsumer));
    assert(!registration.renderSimilarityRows(image, {.scale = nan}, rowConsumer));
    assert(!registration.renderAffineRows(image, {.a = nan}, rowConsumer));
    photonstack::DistortionTransform invalidTransform;
    invalidTransform.influenceRadius = nan;
    assert(!registration.renderDistortionRows(image, invalidTransform, rowConsumer));
    assert(registration.applyTranslation(image, {.dx = nan}).empty());
    assert(registration.applySimilarity(image, {.scale = nan}).empty());
    assert(registration.applyAffine(image, {.a = nan}).empty());
    assert(registration.applyDistortion(image, invalidTransform).empty());

    auto nonFiniteRegistrationImage = image;
    nonFiniteRegistrationImage.pixels.front() = infinity;
    const auto nonFiniteRegistration = registration.estimateTranslation(
        nonFiniteRegistrationImage, image, {});
    assert(!nonFiniteRegistration.ok);
    assert(nonFiniteRegistration.errorCode == "ImageBufferInvalid");
    assert(registration.applySimilarity(nonFiniteRegistrationImage, {}).empty());
    assert(!registration.renderSimilarityRows(nonFiniteRegistrationImage, {}, rowConsumer));

    photonstack::StackOptions stackOptions;
    stackOptions.method = photonstack::StackMethod::SigmaClip;
    stackOptions.sigma.sigmaLow = nan;
    expectArgumentInvalid(photonstack::Stacker().stack({"missing-input.png"}, stackOptions));
    stackOptions = {};
    stackOptions.method = photonstack::StackMethod::PercentileClip;
    stackOptions.percentile.high = infinity;
    expectArgumentInvalid(photonstack::Stacker().stack({"missing-input.png"}, stackOptions));
    stackOptions = {};
    stackOptions.raw.demosaicQuality = static_cast<photonstack::RawDemosaicQuality>(999);
    expectArgumentInvalid(photonstack::Stacker().stack({"missing-input.fits"}, stackOptions));

    photonstack::MasterFrameOptions masterOptions;
    masterOptions.raw.linearOutput = false;
    expectArgumentInvalid(photonstack::MasterFrameBuilder().build({"missing-input.fits"}, masterOptions));

    photonstack::ImageReadOptions readOptions;
    readOptions.raw.exposureBias = nan;
    readOptions.raw.linearOutput = false;
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.nef", readOptions));
    auto rawImage = image;
    photonstack::applyRawDecodeOptions(rawImage, readOptions.raw);
    assert(rawImage.pixels == image.pixels);
    readOptions = {};
    readOptions.raw.whiteBalanceMode = static_cast<photonstack::RawWhiteBalanceMode>(999);
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.nef", readOptions));
    rawImage = image;
    photonstack::applyRawDecodeOptions(rawImage, readOptions.raw);
    assert(rawImage.pixels == image.pixels);
    readOptions = {};
    readOptions.raw.manualWhiteBalanceTemperature = 1999.0F;
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.nef", readOptions));
    readOptions = {};
    readOptions.raw.manualWhiteBalanceTint = 151.0F;
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.nef", readOptions));
    readOptions = {};
    readOptions.raw.manualBlackLevel = -0.01F;
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.nef", readOptions));
    readOptions = {};
    readOptions.raw.exposureBias = 5.01F;
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.nef", readOptions));

    photonstack::ImageBuffer linearRawAdjustment;
    linearRawAdjustment.width = 1;
    linearRawAdjustment.height = 1;
    linearRawAdjustment.channels = 4;
    linearRawAdjustment.colorEncoding = photonstack::ColorEncoding::Linear;
    linearRawAdjustment.pixels = {0.5F, 0.25F, 0.125F, 1.0F};
    auto adjustedRaw = linearRawAdjustment;
    photonstack::applyRawDecodeOptions(adjustedRaw, {});
    assert(adjustedRaw.colorEncoding == photonstack::ColorEncoding::Linear);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        assert(std::abs(adjustedRaw.pixels[channel] - linearRawAdjustment.pixels[channel]) < 1.0e-6F);
    }
    auto hiddenRawAdjustment = linearRawAdjustment;
    hiddenRawAdjustment.pixels = {nan, infinity, 1.0F, 0.0F};
    photonstack::applyRawDecodeOptions(hiddenRawAdjustment, {});
    assert(hiddenRawAdjustment.pixels[0] == 0.0F);
    assert(hiddenRawAdjustment.pixels[1] == 0.0F);
    assert(hiddenRawAdjustment.pixels[2] == 0.0F);
    assert(hiddenRawAdjustment.pixels[3] == 0.0F);
    auto malformedRawAdjustment = linearRawAdjustment;
    malformedRawAdjustment.width = 2;
    const auto malformedRawPixels = malformedRawAdjustment.pixels;
    photonstack::applyRawDecodeOptions(malformedRawAdjustment, {});
    assert(malformedRawAdjustment.pixels == malformedRawPixels);

    photonstack::RawDecodeOptions displayRawOptions;
    displayRawOptions.linearOutput = false;
    adjustedRaw = linearRawAdjustment;
    photonstack::applyRawDecodeOptions(adjustedRaw, displayRawOptions);
    assert(adjustedRaw.colorEncoding == photonstack::ColorEncoding::SRGB);
    assert(std::abs(adjustedRaw.pixels[0] - 0.735357F) < 1.0e-4F);

    auto srgbRawAdjustment = linearRawAdjustment;
    srgbRawAdjustment.colorEncoding = photonstack::ColorEncoding::SRGB;
    adjustedRaw = srgbRawAdjustment;
    photonstack::applyRawDecodeOptions(adjustedRaw, {});
    assert(adjustedRaw.colorEncoding == photonstack::ColorEncoding::Linear);
    assert(std::abs(adjustedRaw.pixels[0] - 0.214041F) < 1.0e-4F);

    photonstack::RawDecodeOptions manualBlackOptions;
    manualBlackOptions.blackLevelMode = photonstack::RawBlackLevelMode::Manual;
    manualBlackOptions.manualBlackLevel = 0.1F;
    adjustedRaw = linearRawAdjustment;
    photonstack::applyRawDecodeOptions(adjustedRaw, manualBlackOptions);
    assert(std::abs(adjustedRaw.pixels[0] - 0.4F) < 1.0e-6F);
    assert(std::abs(adjustedRaw.pixels[1] - 0.15F) < 1.0e-6F);
    assert(std::abs(adjustedRaw.pixels[2] - 0.025F) < 1.0e-6F);

    photonstack::ImageBuffer coveredRawAdjustment;
    coveredRawAdjustment.width = 3;
    coveredRawAdjustment.height = 1;
    coveredRawAdjustment.channels = 4;
    coveredRawAdjustment.colorEncoding = photonstack::ColorEncoding::Linear;
    coveredRawAdjustment.pixels = {
        0.2F, 0.4F, 0.8F, 1.0F,
        0.2F, 0.4F, 0.8F, 1.0F,
        1.0F, 0.0F, 0.0F, 0.0F,
    };
    photonstack::RawDecodeOptions autoWhiteBalanceOptions;
    autoWhiteBalanceOptions.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Auto;
    photonstack::applyRawDecodeOptions(coveredRawAdjustment, autoWhiteBalanceOptions);
    const float expectedNeutral = (0.2F + 0.4F + 0.8F) / 3.0F;
    for (std::size_t pixel = 0; pixel < 2; ++pixel) {
        const auto offset = pixel * 4;
        assert(std::abs(coveredRawAdjustment.pixels[offset] - expectedNeutral) < 1.0e-5F);
        assert(std::abs(coveredRawAdjustment.pixels[offset + 1] - expectedNeutral) < 1.0e-5F);
        assert(std::abs(coveredRawAdjustment.pixels[offset + 2] - expectedNeutral) < 1.0e-5F);
    }
    assert(coveredRawAdjustment.pixels[8] == 0.0F);
    assert(coveredRawAdjustment.pixels[9] == 0.0F);
    assert(coveredRawAdjustment.pixels[10] == 0.0F);

    photonstack::RawDecodeOptions manualWhiteBalanceOptions;
    manualWhiteBalanceOptions.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Manual;
    manualWhiteBalanceOptions.manualWhiteBalanceTemperature = 6500.0F;
    adjustedRaw = linearRawAdjustment;
    photonstack::applyRawDecodeOptions(adjustedRaw, manualWhiteBalanceOptions);
    assert(std::abs(adjustedRaw.pixels[0] - linearRawAdjustment.pixels[0]) < 1.0e-6F);
    assert(std::abs(adjustedRaw.pixels[1] - linearRawAdjustment.pixels[1]) < 1.0e-6F);
    assert(std::abs(adjustedRaw.pixels[2] - linearRawAdjustment.pixels[2]) < 1.0e-6F);
    manualWhiteBalanceOptions.manualWhiteBalanceTemperature = 25000.0F;
    auto warmLimitRaw = linearRawAdjustment;
    photonstack::applyRawDecodeOptions(warmLimitRaw, manualWhiteBalanceOptions);
    manualWhiteBalanceOptions.manualWhiteBalanceTemperature = 50000.0F;
    auto extendedTemperatureRaw = linearRawAdjustment;
    photonstack::applyRawDecodeOptions(extendedTemperatureRaw, manualWhiteBalanceOptions);
    assert(std::abs(warmLimitRaw.pixels[0] - extendedTemperatureRaw.pixels[0]) > 1.0e-4F ||
           std::abs(warmLimitRaw.pixels[2] - extendedTemperatureRaw.pixels[2]) > 1.0e-4F);

    photonstack::ImageBuffer brightRawAdjustment = linearRawAdjustment;
    brightRawAdjustment.pixels[0] = std::numeric_limits<float>::max();
    photonstack::RawDecodeOptions brightRawOptions;
    brightRawOptions.exposureBias = 5.0F;
    photonstack::applyRawDecodeOptions(brightRawAdjustment, brightRawOptions);
    assert(std::isfinite(brightRawAdjustment.pixels[0]));
    assert(brightRawAdjustment.pixels[0] == std::numeric_limits<float>::max());
    brightRawAdjustment = linearRawAdjustment;
    brightRawAdjustment.pixels[0] = std::numeric_limits<float>::max();
    brightRawOptions = {};
    brightRawOptions.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Manual;
    brightRawOptions.manualWhiteBalanceTemperature = 2000.0F;
    brightRawOptions.manualWhiteBalanceTint = -150.0F;
    photonstack::applyRawDecodeOptions(brightRawAdjustment, brightRawOptions);
    assert(std::isfinite(brightRawAdjustment.pixels[0]));
    assert(std::isfinite(brightRawAdjustment.pixels[1]));
    assert(std::isfinite(brightRawAdjustment.pixels[2]));
    readOptions = {};
    readOptions.fits.mode = static_cast<photonstack::FitsDecodeMode>(999);
    expectArgumentInvalid(photonstack::ImageCodec().read("missing-input.fits", readOptions));

    photonstack::ImageWriteOptions writeOptions;
    writeOptions.quality = nan;
    const auto jpegPath = std::filesystem::temp_directory_path() / "photonstack-invalid-quality.jpg";
    expectArgumentInvalid(photonstack::ImageCodec().write(image, jpegPath, writeOptions));
    expectArgumentInvalid(photonstack::ImageCodec().writeRows(
        image, jpegPath,
        [](const photonstack::ImageRowConsumer&) { return true; },
        writeOptions
    ));
    writeOptions = {};
    writeOptions.quality = -0.1F;
    expectArgumentInvalid(photonstack::ImageCodec().write(image, jpegPath, writeOptions));
    writeOptions = {};
    writeOptions.bitDepth = static_cast<photonstack::ImageWriteBitDepth>(999);
    expectArgumentInvalid(photonstack::ImageCodec().write(image, jpegPath, writeOptions));
    writeOptions = {};
    writeOptions.colorSpace = static_cast<photonstack::ImageWriteColorSpace>(999);
    expectArgumentInvalid(photonstack::ImageCodec().writeRows(
        image, jpegPath,
        [](const photonstack::ImageRowConsumer&) { return true; },
        writeOptions
    ));
    const auto fitsPath = std::filesystem::temp_directory_path() / "photonstack-invalid-write-options.fits";
    expectArgumentInvalid(photonstack::ImageCodec().write(image, fitsPath, writeOptions));
    expectArgumentInvalid(photonstack::ImageCodec().writeRows(
        image, fitsPath,
        [](const photonstack::ImageRowConsumer&) { return true; },
        writeOptions
    ));

    photonstack::DrizzleOptions drizzleOptions;
    drizzleOptions.alignment = static_cast<photonstack::DrizzleAlignment>(999);
    expectArgumentInvalid(photonstack::DrizzleStacker().drizzle({"missing-input.tiff"}, drizzleOptions));
    drizzleOptions = {};
    drizzleOptions.fitsDecodeMode = static_cast<photonstack::FitsDecodeMode>(999);
    expectArgumentInvalid(photonstack::DrizzleStacker().drizzle({"missing-input.tiff"}, drizzleOptions));

    photonstack::StackOptions invalidFitsStackOptions;
    invalidFitsStackOptions.fitsDecodeMode = static_cast<photonstack::FitsDecodeMode>(999);
    expectArgumentInvalid(photonstack::Stacker().stack({"missing-input.tiff"}, invalidFitsStackOptions));

    photonstack::MosaicOptions mosaicOptions;
    mosaicOptions.blendMode = static_cast<photonstack::MosaicBlendMode>(999);
    expectArgumentInvalid(photonstack::MosaicBuilder().stitchHorizontal({image}, mosaicOptions));
    mosaicOptions = {};
    mosaicOptions.alignment = photonstack::MosaicAlignment::Auto;
    mosaicOptions.registration.matchTolerance = nan;
    expectArgumentInvalid(photonstack::MosaicBuilder().stitchHorizontal({image}, mosaicOptions));
    mosaicOptions = {};
    mosaicOptions.fitsDecodeMode = static_cast<photonstack::FitsDecodeMode>(999);
    expectArgumentInvalid(photonstack::MosaicBuilder().stitchHorizontal({image}, mosaicOptions));

    expectArgumentInvalid(photonstack::Histogram().luminance(
        image,
        {.bins = std::numeric_limits<std::size_t>::max()}
    ));

    photonstack::ImageBuffer tinyImage;
    tinyImage.width = 1;
    tinyImage.height = 1;
    tinyImage.channels = 4;
    tinyImage.pixels.assign(tinyImage.sampleCount(), 0.5F);
    const auto oversizedResize = photonstack::ImageResizer().resizeToWidth(
        tinyImage,
        std::numeric_limits<std::uint32_t>::max()
    );
    assert(!oversizedResize.ok);
    assert(oversizedResize.errorCode == "ImageDimensionsInvalid");

    photonstack::ImageBuffer overflowingImage;
    overflowingImage.width = std::numeric_limits<std::uint32_t>::max();
    overflowingImage.height = std::numeric_limits<std::uint32_t>::max();
    overflowingImage.channels = std::numeric_limits<std::uint16_t>::max();
    assert(overflowingImage.sampleCount() == std::numeric_limits<std::size_t>::max());
}

} // namespace

int main() {
    testBridgeDefaults();
    testPngInspect();
    testTiffInspect();
    testRasterInspectUsesBoundedMetadataReads();
    testMissingFile();
    testAverageStack();
    testStackIgnoresTransparentFootprints();
    testMedianStack();
    testSigmaClipStack();
    testPercentileClipStack();
    testWinsorizedSigmaStack();
    testWeightedStack();
    testWeightedStackIgnoresTransparentFootprints();
    testStreamingStackRejectsDimensionMismatch();
    testStreamingStackReportsLateReadFailure();
    testRobustStacksIgnoreTransparentFootprintsAndCleanCache();
    testStacksWeightPartialCoverage();
    testSingleFrameStacksNormalizeCoverageWithoutCache();
    testResizeToWidth();
    testResizeIgnoresColorBehindTransparencyAndPreservesMetadata();
    testStarDetection();
    testFrameQualityAnalyzer();
    testFrameNormalizer();
    testCalibration();
    testBackgroundExtraction();
    testCloudDetectionAndRemoval();
    testCloudDetectionRespectsCoverageAndRejectsVisibleNonFinitePixels();
    testCloudDetectionPrefersSmoothSparseHazeOverStarField();
    testCloudDetectionDoesNotGrowBrightHazeIntoMilkyWayStructure();
    testCloudRemovalDoesNotAlterClearDetailInsideComponentBounds();
    testCloudDetectionRejectsSmoothHorizonGradient();
    testCloudRemovalSubtractsColoredVeilWithoutScalingStars();
    testCloudDetectionAndRemovalWithMajorityCoverage();
    testCloudRemovalRecoversWideVeilTouchingImageEdge();
    testCloudDetectionAndRemovalForDarkStarObscuringCloud();
    testCloudDetectionExtendsDarkEdgeCoreIntoWeakCloudFringe();
    testDarkCloudDetectionSurvivesLinearExposureScaling();
    testTemporalCloudDetectionConfirmsWeakDarkBankWithoutBrightStructure();
    testCloudMajorityFallbackRejectsDarkObstructionCross();
    testGridBackgroundExtraction();
    testTranslationRegistration();
    testTranslationRegistrationFindsLargeOffset();
    testAlignedAverageStack();
    testSimilarityRegistration();
    testRegistrationResamplingIgnoresColorBehindTransparencyBeforeStacking();
    testSimilarityRegistrationCorrectsFieldRotation();
    testSimilarityStackKeepsDenseRotatedStarFieldSharp();
    testDistortionStackCorrectsLocalStarFieldWarp();
    testDistortionRegistrationUsesProjectiveBaselineForWideFieldWarp();
    testSimilarityRegistrationFallsBackToTranslation();
    testAffineRegistrationCorrectsEdgeShear();
    testAffineRegistrationPopulatesSimilarityFallbackMatrix();
    testRegistrationRowRenderingMatchesFullFrames();
    testDistortionForwardCoordinatesMatchReverseSampling();
    testStarMask();
    testStarReduction();
    testComaReductionSuppressesElongatedStarTail();
    testComaReductionHonorsDetectionLimit();
    testArtifactTrailRemovalPreservesMeteors();
    testArtifactTrailClassifiesAndSelectivelyRemovesContinuousSatellite();
    testArtifactTrailRecoversCenteredSymmetricSatelliteFlare();
    testArtifactTrailDetectionExcludesMeteorsByDefault();
    testArtifactTrailExtendsFaintMeteorTail();
    testArtifactTrailDetectsDroneLikeShortTrack();
    testArtifactTrailRejectsDenseRandomStarAlignments();
    testArtifactTrailPreservesRotationalStarTrailField();
    testArtifactTrailPreservesFarOutsideRotationalStarTrailField();
    testArtifactTrailFindsLocallyContrastedContinuousSatellite();
    testArtifactTrailFindsNeutralPeriodicSatelliteChain();
    testArtifactTrailRejectsChromaticPeriodicTexture();
    testArtifactTrailRejectsSaturatedPeriodicStars();
    testArtifactTrailPreservesTwoPieceColorfulMeteor();
    testArtifactTrailPreservesLongBrightChromaticMeteor();
    testArtifactTrailRejectsNoisyDenseStarField();
    testArtifactTrailRejectsCrowdedDarkForegroundTexture();
    testMeteorLayerExtractionAndRestore();
    testMeteorLayerRespectsCoverageAndStraightAlpha();
    testMeteorLayerRejectsLargeFrameWeakTaperStarStreaks();
    testArtifactTrailMergesOccludedAirplaneFragments();
    testArtifactTrailDetectsBlinkingColoredAirplaneLights();
    testArtifactTrailDetectsFaintRedBlinkingLights();
    testArtifactTrailDetectsFaintCyanBlinkingLights();
    testArtifactTrailDetectsTwoLowHorizonDottedDroneTrails();
    testArtifactTrailFollowsShortPeriodBottomHorizonDrone();
    testArtifactTrailDetectsAndRemovesOffsetLongPeriodBottomHorizonDrone();
    testArtifactTrailPrioritizesLowHorizonSparseDroneOverTexture();
    testArtifactTrailRemovalCleansLowHorizonDottedDrone();
    testArtifactTrailFollowsAndRemovesCurvedHorizonDrone();
    testHistogram();
    testStretch();
    testStretchPreservesColorRatio();
    testAutoStretch();
    testAutoStretchExactOrderStatistics();
    testParallelStretchMatchesSerialTiles();
    testCurves();
    testLocalContrast();
    testNoiseReduction();
    testSharpen();
    testColorAdjuster();
    testDeconvolution();
    testScientificNeighborhoodFiltersRespectCoverageAndRange();
    testScientificDisplayBridgeProjectsOnlyDisplayCorrections();
    testScientificCloudRemovalBridgePreservesDynamicRange();
    testScientificArtifactRemovalBridgePreservesDynamicRange();
    testDrizzleStacker();
    testMultiImageOperationsRejectMixedColorEncodings();
    testDrizzleDistortionAlignmentSharpensWideFieldEdges();
    testMasterFrameBuilder();
    testMosaicBuilder();
    testMosaicBuilderProgress();
    testMosaicBuilderGridLayout();
    testMosaicBuilderAutoAlignment();
    testMosaicBuilderAutoAffineGeometry();
    testMosaicBuilderAutoAffineExposureMatching();
    testMosaicBuilderAutoGridUsesAvailableNeighbors();
    testMosaicBuilderPropagatesFallbackTrust();
    testMosaicBuilderExposureMatching();
    testMosaicBuilderPreservesCoverageAndReportsExposure();
    testMosaicCylindricalProjectionIgnoresColorBehindTransparency();
    testMosaicPreviewUsesCommonScale();
    testMosaicBuilderMultibandBlendMode();
    testMosaicBuilderPreservesScientificRangeWithoutOverflow();
    testMosaicBuilderKeepsSubpixelPlacement();
    testBridgeMosaicPaths();
    testBridgeCurvePreviewPath();
    testWriteTiff16();
    testWriteOptionsCanForceTiff8Bit();
    testImageCodecStreamsRowsWithoutChangingPixelsOrLeavingCache();
    testImageCodecReadReportsDecodedBitDepth();
    testImageCodecConvertsColorEncodingAndReportsEffectiveBitDepth();
    testImageCodecRejectsUnknownOutputAndFlattensJpegTransparencyToBlack();
    testRasterWritersSanitizeNonFiniteSamples();
    testImageCodecAppliesMetadataOrientation();
    testFitsInspectReadWrite();
    testFitsWriteConvertsSRGBToLinearAndPreservesAlpha();
    testFitsScientificReadPreservesPhysicalValues();
    testFitsScientificValuesFlowThroughStackAndDrizzle();
    testFitsNonFinitePixelsCanBePreservedOrMasked();
    testFitsRejectsMalformedNumericHeaders();
    testFitsStreamingHeaderAndPayloadIO();
    testTileProcessor();
    testProcessingOptionsRejectNonFiniteValues();
    return 0;
}
