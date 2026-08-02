#include "AppleRawDecoder.hpp"

#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
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

} // namespace

ImageReadResult decodeAppleRawFloat(const std::filesystem::path& path, const RawDecodeOptions& options) {
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

            NSDictionary<CIContextOption, id>* contextOptions = @{
                kCIContextWorkingFormat : @(kCIFormatRGBAf),
                kCIContextCacheIntermediates : @NO,
                kCIContextOutputPremultiplied : @NO,
            };
            id<MTLDevice> metalDevice = MTLCreateSystemDefaultDevice();
            CIContext* context = metalDevice == nil
                                     ? [CIContext contextWithOptions:contextOptions]
                                     : [CIContext contextWithMTLDevice:metalDevice options:contextOptions];
            if (context == nil && metalDevice != nil) {
                NSDictionary<CIContextOption, id>* compatibleMetalOptions = @{
                    kCIContextCacheIntermediates : @NO,
                    kCIContextOutputPremultiplied : @NO,
                };
                context = [CIContext contextWithMTLDevice:metalDevice options:compatibleMetalOptions];
            }
            if (context == nil) {
                NSDictionary<CIContextOption, id>* compatibleCPUOptions = @{
                    kCIContextCacheIntermediates : @NO,
                    kCIContextOutputPremultiplied : @NO,
                };
                context = [CIContext contextWithOptions:compatibleCPUOptions];
            }
            if (context == nil) {
                CGColorSpaceRelease(outputColorSpace);
                return rawDecodeError("RawDecodeFailed", "Could not create the Core Image RAW rendering context");
            }

            @try {
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
            [context clearCaches];

            for (std::size_t pixel = 0; pixel < result.image.pixelCount(); ++pixel) {
                const auto offset = pixel * result.image.channels;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    float& value = result.image.pixels[offset + channel];
                    if (!std::isfinite(value)) {
                        return rawDecodeError("RawDecodeFailed", "Core Image RAW decoder produced non-finite color data");
                    }
                    value = options.linearOutput ? std::max(0.0F, value) : std::clamp(value, 0.0F, 1.0F);
                }
                float& alpha = result.image.pixels[offset + 3];
                if (!std::isfinite(alpha)) {
                    return rawDecodeError("RawDecodeFailed", "Core Image RAW decoder produced a non-finite alpha channel");
                }
                alpha = std::clamp(alpha, 0.0F, 1.0F);
            }

            result.ok = true;
            return result;
        } @catch (NSException* exception) {
            return rawDecodeError("RawDecodeFailed", exceptionMessage(exception));
        }
    }
}

} // namespace photonstack
