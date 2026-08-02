#pragma once

#include <cstdint>
#include <string>

namespace photonstack {

enum class ImageFormat {
    Unknown,
    PNG,
    JPEG,
    HEIF,
    TIFF,
    FITS,
    RAW,
};

struct ImageMetadata {
    std::string path;
    ImageFormat format = ImageFormat::Unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerChannel = 0;
    std::string cameraMake;
    std::string cameraModel;
    std::string lensModel;
    std::string captureDate;
    std::string colorModel;
    std::string colorProfile;
    std::string rawDecoder;
    std::string whiteBalance;
    std::string exposureBias;
    double exposureTimeSeconds = 0.0;
    double fNumber = 0.0;
    double focalLengthMM = 0.0;
    std::uint32_t iso = 0;
    std::uint32_t orientation = 0;
};

struct InspectResult {
    bool ok = false;
    ImageMetadata metadata;
    std::string errorCode;
    std::string message;
};

std::string toString(ImageFormat format);

} // namespace photonstack
