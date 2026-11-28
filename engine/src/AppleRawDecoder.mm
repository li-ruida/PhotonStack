#include "photonstack/StageProfile.hpp"
#include "photonstack/ParallelRanges.hpp"
#include "AppleRawDecoder.hpp"

#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace photonstack {
namespace {

ImageReadResult rawDecodeError(std::string code, std::string message) {
    ImageReadResult result;
    result.ok = false;
    result.errorCode = std::move(code);
    result.message = std::move(message);
    return result;
}

std::string exceptionMessage(NSException* exception) {
    if (exception.reason == nil) {
        return "Core Image raised an exception while decoding RAW";
    }
    const char* text = exception.reason.UTF8String;
    return text == nullptr ? "Core Image raised an exception while decoding RAW" : std::string(text);
}

struct RawRenderContext {
    CIContext* context;
    bool metal;
};

RawRenderContext makeRawContext(bool useGPU) {
    CIContext* context = nil;
    bool explicitMetal = false;
    NSDictionary<CIContextOption, id>* contextOptions = @{
        kCIContextWorkingFormat : @(kCIFormatRGBAf),
        kCIContextCacheIntermediates : @NO,
        kCIContextOutputPremultiplied : @NO,
    };
    id<MTLDevice> metalDevice = useGPU ? MTLCreateSystemDefaultDevice() : nil;
    explicitMetal = metalDevice != nil;
    if (!useGPU) {
        NSDictionary<CIContextOption, id>* softwareOptions = @{
            kCIContextWorkingFormat : @(kCIFormatRGBAf),
            kCIContextCacheIntermediates : @NO,
            kCIContextOutputPremultiplied : @NO,
            kCIContextUseSoftwareRenderer : @YES,
        };
        context = [CIContext contextWithOptions:softwareOptions];
    } else {
        context = metalDevice == nil
            ? [CIContext contextWithOptions:contextOptions]
            : [CIContext contextWithMTLDevice:metalDevice options:contextOptions];
    }
    if (context == nil && metalDevice != nil) {
        NSDictionary<CIContextOption, id>* compatibleMetalOptions = @{
            kCIContextCacheIntermediates : @NO,
            kCIContextOutputPremultiplied : @NO,
        };
        context = [CIContext contextWithMTLDevice:metalDevice options:compatibleMetalOptions];
    }
    if (context == nil && useGPU) {
        NSDictionary<CIContextOption, id>* compatibleCPUOptions = @{
            kCIContextCacheIntermediates : @NO,
            kCIContextOutputPremultiplied : @NO,
        };
        explicitMetal = false;
        context = [CIContext contextWithOptions:compatibleCPUOptions];
    }
    return {context, explicitMetal};
}

const RawRenderContext& sharedRawContext(bool useGPU) {
    if (useGPU) {
        static const auto accelerated = makeRawContext(true);
        return accelerated;
    }
    static const auto software = makeRawContext(false);
    return software;
}

} // namespace

ImageReadResult decodeAppleRawFloat(const std::filesystem::path& path, const RawDecodeOptions& options) {
    const detail::StageScope timer(detail::Stage::RawDecode);
    @autoreleasepool {
        @try {
            const auto pathString = path.string();
            NSString* filePath = [[NSString alloc] initWithBytes:pathString.data()
                                                          length:pathString.size()
                                                        encoding:NSUTF8StringEncoding];
            if (filePath == nil) {
                return rawDecodeError("InputPathInvalid", "Could not convert RAW path to an Apple file URL");
            }

            CIRAWFilter* rawFilter = [CIRAWFilter filterWithImageURL:[NSURL fileURLWithPath:filePath]];
            if (rawFilter == nil) {
                return rawDecodeError("RawDecodeUnavailable", "Core Image could not create a RAW decoder");
            }

            rawFilter.draftModeEnabled = options.demosaicQuality == RawDemosaicQuality::Fast;
            rawFilter.scaleFactor = 1.0F;
            rawFilter.exposure = options.exposureBias;
            if (options.whiteBalanceMode == RawWhiteBalanceMode::Daylight) {
                rawFilter.neutralTemperature = 5500.0F;
                rawFilter.neutralTint = 0.0F;
            } else if (options.whiteBalanceMode == RawWhiteBalanceMode::Manual) {
                rawFilter.neutralTemperature = options.manualWhiteBalanceTemperature;
                rawFilter.neutralTint = options.manualWhiteBalanceTint;
            }
            if (options.linearOutput) {
                rawFilter.boostAmount = 0.0F;
                if (rawFilter.localToneMapSupported) {
                    rawFilter.localToneMapAmount = 0.0F;
                }
                rawFilter.extendedDynamicRangeAmount = 0.0F;
            }

            CIImage* outputImage = rawFilter.outputImage;
            if (outputImage == nil) {
                return rawDecodeError("RawDecodeFailed", "Core Image RAW decoder produced no image");
            }

            const CGRect extent = CGRectIntegral(outputImage.extent);
            if (CGRectIsEmpty(extent) || CGRectIsInfinite(extent) || !std::isfinite(extent.size.width) ||
                !std::isfinite(extent.size.height) || extent.size.width > std::numeric_limits<std::uint32_t>::max() ||
                extent.size.height > std::numeric_limits<std::uint32_t>::max()) {
                return rawDecodeError("RawDecodeFailed", "Core Image RAW decoder produced invalid dimensions");
            }

            const auto width = static_cast<std::uint32_t>(extent.size.width);
            const auto height = static_cast<std::uint32_t>(extent.size.height);
            if (width == 0 || height == 0 ||
                static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / height / 4U) {
                return rawDecodeError("RawDecodeFailed",
                                      "Core Image RAW dimensions exceed the addressable buffer size");
            }

            ImageReadResult result;
            result.backend = ImageReadBackend::AppleRaw;
            result.image.width = width;
            result.image.height = height;
            result.image.channels = 4;
            result.image.format = PixelFormat::Float32RGBA;
            result.image.colorEncoding = options.linearOutput ? ColorEncoding::Linear : ColorEncoding::SRGB;
            result.image.sourceBitsPerChannel = 16;
            try {
                result.image.pixels.resize(result.image.sampleCount());
            } catch (const std::bad_alloc&) {
                return rawDecodeError("ImageAllocationFailed", "Could not allocate the Float32 RAW image buffer");
            }

            CGColorSpaceRef outputColorSpace =
                CGColorSpaceCreateWithName(options.linearOutput ? kCGColorSpaceExtendedLinearSRGB : kCGColorSpaceSRGB);
            if (outputColorSpace == nullptr) {
                return rawDecodeError("RawDecodeFailed", "Could not create the RAW output color space");
            }

            CIContext* context = nil;
            @try {
                const auto& renderer = sharedRawContext(options.useGPU);
                context = renderer.context;
                const bool explicitMetal = renderer.metal;
                if (context == nil) {
                    return rawDecodeError("RawDecodeFailed", "Could not create the Core Image RAW rendering context");
                }

                detail::recordBackend(explicitMetal ? "metal" : options.useGPU ? "core-image-auto" : "cpu",
                                      options.useGPU && !explicitMetal ? "explicit Metal context unavailable" : "");
                const auto rowBytes = static_cast<ptrdiff_t>(width) * 4 * static_cast<ptrdiff_t>(sizeof(float));
                constexpr std::uint32_t renderRows = 512;
                for (std::uint32_t outputRow = 0; outputRow < height; outputRow += renderRows) {
                    const auto rows = std::min(renderRows, height - outputRow);
                    const CGRect bounds = CGRectMake(
                        extent.origin.x,
                        CGRectGetMaxY(extent) - static_cast<CGFloat>(outputRow + rows),
                        extent.size.width,
                        static_cast<CGFloat>(rows)
                    );
                    [context render:outputImage
                           toBitmap:result.image.pixels.data() + static_cast<std::size_t>(outputRow) * width * 4U
                           rowBytes:rowBytes
                             bounds:bounds
                             format:kCIFormatRGBAf
                         colorSpace:outputColorSpace];
                }
            } @finally {
                CGColorSpaceRelease(outputColorSpace);
            }

            std::atomic<std::size_t> firstInvalid{result.image.sampleCount()};
            detail::parallelRanges(result.image.pixelCount(), 65536, [&](std::size_t first, std::size_t last) noexcept {
                auto invalid = result.image.sampleCount();
                for (auto pixel = first; pixel < last; ++pixel) {
                    const auto offset = pixel * result.image.channels;
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        float& value = result.image.pixels[offset + channel];
                        if (!std::isfinite(value)) invalid = std::min(invalid, offset + channel);
                        value = options.linearOutput ? std::max(0.0F, value) : std::clamp(value, 0.0F, 1.0F);
                    }
                    float& alpha = result.image.pixels[offset + 3];
                    if (!std::isfinite(alpha)) invalid = std::min(invalid, offset + 3);
                    alpha = std::clamp(alpha, 0.0F, 1.0F);
                }
                auto previous = firstInvalid.load(std::memory_order_relaxed);
                while (invalid < previous && !firstInvalid.compare_exchange_weak(previous, invalid, std::memory_order_relaxed)) {}
            });
            if (const auto invalid = firstInvalid.load(); invalid < result.image.sampleCount())
                return rawDecodeError("RawDecodeFailed", invalid % 4 == 3 ?
                    "Core Image RAW decoder produced a non-finite alpha channel" :
                    "Core Image RAW decoder produced non-finite color data");

            result.ok = true;
            return result;
        } @catch (NSException* exception) {
            return rawDecodeError("RawDecodeFailed", exceptionMessage(exception));
        }
    }
}

} // namespace photonstack
