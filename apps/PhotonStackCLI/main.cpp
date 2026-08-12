#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

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
#include "photonstack/RawCodec.hpp"
#include "photonstack/Registration.hpp"
#include "photonstack/ScientificDisplayBridge.hpp"
#include "photonstack/Sharpen.hpp"
#include "photonstack/Stacker.hpp"
#include "photonstack/StarDetector.hpp"
#include "photonstack/StarMask.hpp"
#include "photonstack/StarReducer.hpp"
#include "photonstack/Stretch.hpp"
#include "photonstack/Version.hpp"

namespace {

std::string jsonEscape(const std::string& value);
void printError(const std::string& code, const std::string& message);
void printProgressEvent(
    const std::string& task,
    const std::string& command,
    const std::string& stage,
    double progress,
    std::optional<std::size_t> step = std::nullopt,
    std::optional<std::size_t> total = std::nullopt
);
bool parseOnOff(const std::vector<std::string>& args, std::size_t& index, const std::string& name, bool& target);
bool parseRawDecodeArgument(
    const std::vector<std::string>& args,
    std::size_t& index,
    photonstack::RawDecodeOptions& options
);
std::string rawWhiteBalanceName(photonstack::RawWhiteBalanceMode mode);
std::string rawBlackLevelName(photonstack::RawBlackLevelMode mode);
std::string rawDemosaicName(photonstack::RawDemosaicQuality quality);
int runCommand(const std::vector<std::string>& args);
int runCommandImpl(const std::vector<std::string>& args);

PhotonStackRawWhiteBalanceMode bridgeRawWhiteBalanceMode(photonstack::RawWhiteBalanceMode mode) {
    switch (mode) {
    case photonstack::RawWhiteBalanceMode::Auto:
        return PHOTONSTACK_RAW_WHITE_BALANCE_AUTO;
    case photonstack::RawWhiteBalanceMode::Daylight:
        return PHOTONSTACK_RAW_WHITE_BALANCE_DAYLIGHT;
    case photonstack::RawWhiteBalanceMode::Manual:
        return PHOTONSTACK_RAW_WHITE_BALANCE_MANUAL;
    case photonstack::RawWhiteBalanceMode::Camera:
    default:
        return PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA;
    }
}

PhotonStackRawBlackLevelMode bridgeRawBlackLevelMode(photonstack::RawBlackLevelMode mode) {
    switch (mode) {
    case photonstack::RawBlackLevelMode::Auto:
        return PHOTONSTACK_RAW_BLACK_LEVEL_AUTO;
    case photonstack::RawBlackLevelMode::Manual:
        return PHOTONSTACK_RAW_BLACK_LEVEL_MANUAL;
    case photonstack::RawBlackLevelMode::Camera:
    default:
        return PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA;
    }
}

PhotonStackRawDemosaicQuality bridgeRawDemosaicQuality(photonstack::RawDemosaicQuality quality) {
    switch (quality) {
    case photonstack::RawDemosaicQuality::Fast:
        return PHOTONSTACK_RAW_DEMOSAIC_FAST;
    case photonstack::RawDemosaicQuality::Balanced:
        return PHOTONSTACK_RAW_DEMOSAIC_BALANCED;
    case photonstack::RawDemosaicQuality::High:
    default:
        return PHOTONSTACK_RAW_DEMOSAIC_HIGH;
    }
}

PhotonStackCurveChannel bridgeCurveChannel(photonstack::CurveChannel channel) {
    switch (channel) {
    case photonstack::CurveChannel::Red:
        return PHOTONSTACK_CURVE_CHANNEL_RED;
    case photonstack::CurveChannel::Green:
        return PHOTONSTACK_CURVE_CHANNEL_GREEN;
    case photonstack::CurveChannel::Blue:
        return PHOTONSTACK_CURVE_CHANNEL_BLUE;
    case photonstack::CurveChannel::Luminance:
        return PHOTONSTACK_CURVE_CHANNEL_LUMINANCE;
    case photonstack::CurveChannel::RGB:
    default:
        return PHOTONSTACK_CURVE_CHANNEL_RGB;
    }
}

const char* bridgeImageReadBackendName(PhotonStackImageReadBackend backend) {
    switch (backend) {
    case PHOTONSTACK_IMAGE_READ_BACKEND_IMAGEIO:
        return "imageio";
    case PHOTONSTACK_IMAGE_READ_BACKEND_APPLE_RAW:
        return "apple-raw";
    case PHOTONSTACK_IMAGE_READ_BACKEND_FITS:
        return "fits";
    case PHOTONSTACK_IMAGE_READ_BACKEND_UNKNOWN:
    default:
        return "unknown";
    }
}

float parseFiniteFloat(const std::string& text) {
    std::size_t parsed = 0;
    const float value = std::stof(text, &parsed);
    if (parsed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("Expected a finite floating-point value");
    }
    return value;
}

template <typename T>
T parseUnsignedInteger(const std::string& text) {
    static_assert(std::is_unsigned_v<T>);
    T value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [parsedEnd, error] = std::from_chars(begin, end, value);
    if (error == std::errc::result_out_of_range) {
        throw std::out_of_range("Unsigned integer is out of range");
    }
    if (text.empty() || error != std::errc{} || parsedEnd != end) {
        throw std::invalid_argument("Expected an unsigned integer");
    }
    return value;
}

const char* fitsDecodeModeName(photonstack::FitsDecodeMode mode) {
    switch (mode) {
    case photonstack::FitsDecodeMode::Scientific:
        return "scientific";
    case photonstack::FitsDecodeMode::DisplayNormalized:
    default:
        return "display";
    }
}

bool parseFitsValuesArgument(const std::vector<std::string>& args, std::size_t& index,
                             photonstack::FitsDecodeOptions& options) {
    if (index + 1 >= args.size()) {
        printError("ArgumentMissing", "--fits-values requires display or scientific");
        return false;
    }
    const auto value = args[++index];
    if (value == "display") {
        options.mode = photonstack::FitsDecodeMode::DisplayNormalized;
    } else if (value == "scientific") {
        options.mode = photonstack::FitsDecodeMode::Scientific;
    } else {
        printError("ArgumentInvalid", "--fits-values must be display or scientific");
        return false;
    }
    return true;
}

photonstack::ImageReadOptions scientificFitsReadOptions() {
    photonstack::ImageReadOptions options;
    options.fits.mode = photonstack::FitsDecodeMode::Scientific;
    options.fits.maskNonFinitePixels = true;
    return options;
}

void printUsage() {
    std::cout << "PhotonStack CLI\n\n"
              << "Usage:\n"
              << "  photonstack --version\n"
              << "  photonstack inspect <image>\n"
              << "  photonstack convert --input <image> --output <image> "
                 "[--bit-depth auto|8|16] [--color-space srgb|linear-srgb] [--quality 0.92] "
                 "[--fits-values display|scientific] "
                 "[--raw-white-balance camera|auto|daylight|manual] [--raw-exposure-bias stops] "
                 "[--raw-temperature kelvin] [--raw-tint value] [--raw-black-value value] "
                 "[--raw-black-level camera|auto|manual] [--raw-demosaic fast|high] "
                 "[--raw-linear on|off]\n"
              << "  photonstack run <workflow-file>\n"
              << "  photonstack fits inspect <image>\n"
              << "  photonstack fits convert --input <fits> --output <image> "
                 "[--fits-values display|scientific]\n"
              << "  photonstack raw inspect <raw>\n"
              << "  photonstack raw convert --input <raw> --output <image> "
                 "[--bit-depth auto|8|16] [--color-space srgb|linear-srgb] [--quality 0.92] "
                 "[--raw-white-balance camera|auto|daylight|manual] [--raw-exposure-bias stops] "
                 "[--raw-temperature kelvin] [--raw-tint value] [--raw-black-value value] "
                 "[--raw-black-level camera|auto|manual] [--raw-demosaic fast|high] "
                 "[--raw-linear on|off]\n"
              << "  photonstack raw adjust --input <image> --output <image> "
                 "[--raw-white-balance camera|auto|daylight|manual] [--raw-exposure-bias stops] "
                 "[--raw-temperature kelvin] [--raw-tint value] [--raw-black-value value] "
                 "[--raw-black-level camera|auto|manual] [--raw-linear on|off]\n"
              << "  photonstack preview --input <image> --output <image> [--width 1600] "
                 "[--curve-points 0:0,0.5:0.6,1:1] [--curve-channel rgb|red|green|blue|luminance] "
                 "[--raw-white-balance camera|auto|daylight|manual] [--raw-exposure-bias stops] "
                 "[--raw-temperature kelvin] [--raw-tint value] [--raw-black-value value] "
                 "[--raw-black-level camera|auto|manual] [--raw-demosaic fast|high] "
                 "[--raw-linear on|off]\n"
              << "  photonstack crop --input <image> --output <image> [--x 0 --y 0 --width w --height h] "
                 "[--aspect 16:9 --margin 0.08]\n"
              << "  photonstack calibrate --light <image> --output <image> [--dark <image>] [--bias <image>] "
                 "[--flat <image>] [--dark-bias included|removed] [--flat-bias included|removed] "
                 "[--raw-white-balance camera|auto|daylight|manual] "
                 "[--raw-temperature kelvin] [--raw-tint value] [--raw-black-value value] "
                 "[--raw-exposure-bias stops] [--raw-black-level camera|auto|manual] "
                 "[--raw-demosaic fast|high]\n"
              << "  photonstack background --input <image> --output <image> [--model global|grid] "
                 "[--mode subtract|divide] [--strength 1] [--preserve-brightness on|off] "
                 "[--protect-bright-targets on|off]\n"
              << "  photonstack clouds detect --input <image> [--grid-x 32] [--grid-y 20]\n"
              << "  photonstack clouds temporal-detect --input <image> --reference <image> --reference <image> [...]\n"
              << "  photonstack clouds temporal-remove --input <image> --reference <image> --reference <image> [...] --output <image> "
                 "[--selected-indices 0,2] [--strength 0.65] [--feather-radius 24]\n"
              << "  photonstack clouds remove --input <image> --output <image> "
                 "[--selected-indices 0,2] [--strength 0.65] [--feather-radius 24]\n"
              << "  photonstack histogram --input <image> [--bins 256]\n"
              << "  photonstack quality --input <image> [--sigma-threshold 3.0]\n"
              << "  photonstack normalize --input <image> --output <image> [--model global|local] "
                 "[--reference <image>] [--target-background 0.25] [--target-scale 1] [--grid-x 16] [--grid-y 12]\n"
              << "  photonstack stretch --input <image> --output <image> [--auto] [--black 0] [--mid 0.5] "
                 "[--white 1] [--arcsinh 0] [--target-background 0.25] [--shadows-sigma 2.8]\n"
              << "  photonstack curves --input <image> --output <image> [--points 0:0,0.5:0.6,1:1] "
                 "[--channel rgb|red|green|blue|luminance]\n"
              << "  photonstack local-contrast --input <image> --output <image> [--amount 0.25] [--radius 8]\n"
              << "  photonstack denoise --input <image> --output <image> [--amount 0.5] [--chroma-amount 0.5] "
                 "[--radius 1]\n"
              << "  photonstack sharpen --input <image> --output <image> [--amount 0.35] [--radius 1]\n"
              << "  photonstack deconvolve --input <image> --output <image> [--iterations 8] [--radius 2] [--sigma 1.2]\n"
              << "  photonstack color neutralize --input <image> --output <image> [--strength 1]\n"
              << "  photonstack color saturate --input <image> --output <image> [--amount 0.2]\n"
              << "  photonstack color remove-green --input <image> --output <image> [--amount 0.65] "
                 "[--background-limit 0.32]\n"
              << "  photonstack master --output <image> [--method median|average] "
                 "[--raw-white-balance camera|auto|daylight|manual] [--raw-exposure-bias stops] "
                 "[--raw-temperature kelvin] [--raw-tint value] [--raw-black-value value] "
                 "[--raw-black-level camera|auto|manual] [--raw-demosaic fast|high] <image>...\n"
              << "  photonstack mosaic --output <image> [--overlap-pixels 0] [--projection planar|cylindrical] "
                 "[--layout horizontal|grid] [--columns 2] [--alignment manual|auto] "
                 "[--blend average|feather|multiband] [--exposure-match on|off] [--preview-width 1200] "
                 "[--alignment-width 2000] <image>...\n"
              << "  photonstack register --reference <image> --moving <image> --output <image> "
                 "[--mode translation|similarity|affine|distortion] [--minimum-matches 3]\n"
              << "  photonstack register-batch --reference <image> --output-dir <directory> [--input <directory>] "
                 "[--mode translation|similarity|affine|distortion] [--output-format tiff|png|fits] "
                 "[--include-reference on|off] <image>...\n"
              << "  photonstack stack --input <directory> --output <image> "
                 "[--method average|weighted|median|sigma|winsorized|percentile] "
                 "[--align translation|similarity|affine|distortion] [--minimum-matches 3]\n"
              << "  photonstack stack --output <image> [--method average|weighted|median|sigma|winsorized|percentile] "
                 "[--align translation|similarity|affine|distortion] [--minimum-matches 3] "
                 "<image>...\n"
              << "  photonstack drizzle --output <image> [--scale 2] [--pixfrac 1] "
                 "[--align none|translation|similarity|affine|distortion] "
                 "[--match-tolerance 12] <image>...\n"
              << "  photonstack artifacts detect --input <image> [--preserve-meteors on|off] "
                 "[--include-meteors on|off]\n"
              << "  photonstack artifacts remove --input <image> --output <image> "
                 "[--remove-kinds airplane,drone,satellite] [--selected-indices 0,2] "
                 "[--detected-trails-v1 <encoded>] [--preserve-meteors on|off]\n"
              << "  photonstack artifacts clean-sequence --input <directory> --output-dir <directory> "
                 "[--output-format tiff|png] [--min-weight 0.3] [--sequence-min-length 36] "
                 "[--sequence-min-brightness 0.02] [--recurrence-threshold 2] "
                 "[--remove-kinds airplane,drone,satellite] [--preserve-meteors on|off]\n"
              << "  photonstack meteors extract --input <image> --output <png> "
                 "[--min-confidence 0.35] [--mask-radius 3] [--feather-radius 6]\n"
              << "  photonstack meteors restore --base <stacked> --source <image> --output <image> "
                 "[--opacity 1] [--min-confidence 0.35]\n"
              << "  photonstack meteors compose --base <stacked> --layer <png> --output <image> [--opacity 1]\n"
              << "  photonstack stars detect --input <image> [--sigma-threshold 3.0] [--min-peak 0.05] [--max-stars 50000] [--summary-only]\n"
              << "  photonstack stars mask --input <image> --output <image> [--radius 2] [--large-radius 6] [--layered] [--sigma-threshold 3.0] [--min-peak 0.05] [--max-stars 50000]\n"
              << "  photonstack stars reduce --input <image> --output <image> [--amount 0.35] [--profile-aware] "
                 "[--edge-aware on|off]\n"
              << "  photonstack stars coma --input <image> --output <image> [--amount 0.80] [--radius 10] "
                 "[--eccentricity 0.22] [--edge-aware on|off]\n"
              << "  photonstack help\n\n"
              << "Current formats: PNG, JPEG, HEIF/HEIC, TIFF, FITS, and Apple ImageIO RAW formats on macOS\n";
}

photonstack::ImageWriteResult writeRegisteredFrame(
    const photonstack::ImageCodec& codec, const photonstack::Registration& registration,
    const photonstack::ImageBuffer& moving, const std::filesystem::path& outputPath, const std::string& mode,
    const photonstack::RegistrationResult& registrationResult,
    const photonstack::DistortionTransform& distortionTransform) {
    const auto streamed = codec.writeRows(moving, outputPath, [&](const photonstack::ImageRowConsumer& consumer) {
        if (mode == "distortion") {
            return registration.renderDistortionRows(moving, distortionTransform, consumer);
        }
        if (mode == "affine") {
            return registration.renderAffineRows(moving, registrationResult.affine, consumer);
        }
        if (mode == "similarity") {
            return registration.renderSimilarityRows(moving, registrationResult.transform, consumer);
        }
        return registration.renderTranslationRows(moving, registrationResult.translation, consumer);
    });
    if (streamed.ok || streamed.errorCode != "ImageRowWriteUnsupported") {
        return streamed;
    }

    const auto aligned = mode == "distortion"
                             ? registration.applyDistortion(moving, distortionTransform)
                             : (mode == "affine"
                                    ? registration.applyAffine(moving, registrationResult.affine)
                                    : (mode == "similarity"
                                           ? registration.applySimilarity(moving, registrationResult.transform)
                                           : registration.applyTranslation(moving, registrationResult.translation)));
    return codec.write(aligned, outputPath);
}

int registerImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> referencePath;
    std::optional<std::filesystem::path> movingPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::RegistrationOptions options;
    std::string mode = "translation";

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--reference") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--reference requires an image path");
                return 1;
            }
            referencePath = args[++i];
        } else if (arg == "--moving") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--moving requires an image path");
                return 1;
            }
            movingPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--mode") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--mode requires translation, similarity, or affine");
                return 1;
            }
            mode = args[++i];
            if (mode != "translation" && mode != "similarity" && mode != "affine" && mode != "distortion") {
                printError("ArgumentInvalid", "--mode must be translation, similarity, affine, or distortion");
                return 1;
            }
        } else if (arg == "--match-tolerance") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--match-tolerance requires a positive number");
                return 1;
            }
            try {
                options.matchTolerance = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--match-tolerance must be a positive number");
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                options.starDetection.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--minimum-matches") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--minimum-matches requires a positive integer");
                return 1;
            }
            try {
                options.minimumMatches = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--minimum-matches must be a positive integer");
                return 1;
            }
            if (options.minimumMatches == 0) {
                printError("ArgumentInvalid", "--minimum-matches must be greater than zero");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown register argument: " + arg);
            return 1;
        }
    }

    if (!referencePath.has_value()) {
        printError("ArgumentMissing", "register requires --reference <image>");
        return 1;
    }
    if (!movingPath.has_value()) {
        printError("ArgumentMissing", "register requires --moving <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "register requires --output <image>");
        return 1;
    }

    const photonstack::ImageCodec codec;
    const auto readOptions = scientificFitsReadOptions();
    const auto referenceRead = codec.read(*referencePath, readOptions);
    if (!referenceRead.ok) {
        printError(referenceRead.errorCode, referenceRead.message);
        return 1;
    }
    const auto movingRead = codec.read(*movingPath, readOptions);
    if (!movingRead.ok) {
        printError(movingRead.errorCode, movingRead.message);
        return 1;
    }

    const photonstack::Registration registration;
    photonstack::DistortionTransform distortionTransform;
    const auto registrationResult = mode == "distortion"
                                        ? registration.estimateDistortion(referenceRead.image, movingRead.image, distortionTransform, options)
                                        : (mode == "affine"
                                               ? registration.estimateAffine(referenceRead.image, movingRead.image, options)
                                               : (mode == "similarity"
                                                      ? registration.estimateSimilarity(referenceRead.image, movingRead.image, options)
                                                      : registration.estimateTranslation(referenceRead.image, movingRead.image, options)));
    if (!registrationResult.ok) {
        printError(registrationResult.errorCode, registrationResult.message);
        return 1;
    }

    const auto writeResult = writeRegisteredFrame(codec, registration, movingRead.image, *outputPath, mode,
                                                  registrationResult, distortionTransform);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"register\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"mode\": \"" << jsonEscape(mode) << "\",\n"
              << "  \"dx\": " << registrationResult.translation.dx << ",\n"
              << "  \"dy\": " << registrationResult.translation.dy << ",\n"
              << "  \"scale\": " << registrationResult.transform.scale << ",\n"
              << "  \"rotationRadians\": " << registrationResult.transform.rotationRadians << ",\n"
              << "  \"affineA\": " << registrationResult.affine.a << ",\n"
              << "  \"affineB\": " << registrationResult.affine.b << ",\n"
              << "  \"affineC\": " << registrationResult.affine.c << ",\n"
              << "  \"affineD\": " << registrationResult.affine.d << ",\n"
              << "  \"matches\": " << registrationResult.matches << ",\n"
              << "  \"detectedReferenceStars\": " << registrationResult.detectedReferenceStars << ",\n"
              << "  \"detectedMovingStars\": " << registrationResult.detectedMovingStars << ",\n"
              << "  \"inlierRatio\": " << registrationResult.inlierRatio << ",\n"
              << "  \"usedFallback\": " << (registrationResult.usedFallback ? "true" : "false") << ",\n"
              << "  \"message\": \"" << jsonEscape(registrationResult.message) << "\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int calibrateImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> lightPath;
    std::optional<std::filesystem::path> darkPath;
    std::optional<std::filesystem::path> biasPath;
    std::optional<std::filesystem::path> flatPath;
    std::optional<std::filesystem::path> outputPath;
    auto darkBiasState = photonstack::CalibrationBiasState::Unknown;
    auto flatBiasState = photonstack::CalibrationBiasState::Unknown;
    auto readOptions = scientificFitsReadOptions();

    const auto parseBiasState = [](const std::string& value) -> std::optional<photonstack::CalibrationBiasState> {
        if (value == "included") {
            return photonstack::CalibrationBiasState::Included;
        }
        if (value == "removed") {
            return photonstack::CalibrationBiasState::Removed;
        }
        return std::nullopt;
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--light") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--light requires an image path");
                return 1;
            }
            lightPath = args[++i];
        } else if (arg == "--dark") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--dark requires an image path");
                return 1;
            }
            darkPath = args[++i];
        } else if (arg == "--bias") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--bias requires an image path");
                return 1;
            }
            biasPath = args[++i];
        } else if (arg == "--flat") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--flat requires an image path");
                return 1;
            }
            flatPath = args[++i];
        } else if (arg == "--dark-bias" || arg == "--flat-bias") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", arg + " requires included or removed");
                return 1;
            }
            const auto state = parseBiasState(args[++i]);
            if (!state.has_value()) {
                printError("ArgumentInvalid", arg + " must be included or removed");
                return 1;
            }
            if (arg == "--dark-bias") {
                darkBiasState = *state;
            } else {
                flatBiasState = *state;
            }
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, readOptions.raw)) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown calibrate argument: " + arg);
            return 1;
        }
    }

    if (!lightPath.has_value()) {
        printError("ArgumentMissing", "calibrate requires --light <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "calibrate requires --output <image>");
        return 1;
    }
    if (!readOptions.raw.linearOutput) {
        printError("ArgumentInvalid", "calibrate requires --raw-linear on");
        return 1;
    }
    if (darkBiasState != photonstack::CalibrationBiasState::Unknown && !darkPath.has_value()) {
        printError("ArgumentInvalid", "--dark-bias requires --dark <image>");
        return 1;
    }
    if (flatBiasState != photonstack::CalibrationBiasState::Unknown && !flatPath.has_value()) {
        printError("ArgumentInvalid", "--flat-bias requires --flat <image>");
        return 1;
    }
    if (darkPath.has_value() && biasPath.has_value() &&
        darkBiasState == photonstack::CalibrationBiasState::Unknown) {
        printError(
            "ArgumentMissing",
            "calibrate with both --dark and --bias requires --dark-bias included|removed"
        );
        return 1;
    }
    if (flatPath.has_value() && biasPath.has_value() &&
        flatBiasState == photonstack::CalibrationBiasState::Unknown) {
        printError(
            "ArgumentMissing",
            "calibrate with both --flat and --bias requires --flat-bias included|removed"
        );
        return 1;
    }
    if (flatPath.has_value() && !biasPath.has_value() &&
        flatBiasState == photonstack::CalibrationBiasState::Included) {
        printError("ArgumentMissing", "--flat-bias included requires --bias <image>");
        return 1;
    }

    const photonstack::ImageCodec codec;
    const auto lightRead = codec.read(*lightPath, readOptions);
    if (!lightRead.ok) {
        printError(lightRead.errorCode, lightRead.message);
        return 1;
    }

    photonstack::ImageBuffer dark;
    photonstack::ImageBuffer bias;
    photonstack::ImageBuffer flat;
    photonstack::CalibrationOptions options;
    options.darkBiasState = darkBiasState;
    options.flatBiasState = flatBiasState;
    options.clampNegativeValues = false;

    if (darkPath.has_value()) {
        const auto read = codec.read(*darkPath, readOptions);
        if (!read.ok) {
            printError(read.errorCode, read.message);
            return 1;
        }
        dark = std::move(read.image);
        options.dark = &dark;
    }
    if (biasPath.has_value()) {
        const auto read = codec.read(*biasPath, readOptions);
        if (!read.ok) {
            printError(read.errorCode, read.message);
            return 1;
        }
        bias = std::move(read.image);
        options.bias = &bias;
    }
    if (flatPath.has_value()) {
        const auto read = codec.read(*flatPath, readOptions);
        if (!read.ok) {
            printError(read.errorCode, read.message);
            return 1;
        }
        flat = std::move(read.image);
        options.flat = &flat;
    }

    const photonstack::Calibrator calibrator;
    const auto result = calibrator.calibrate(lightRead.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"calibrate\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"negativeValues\": \"preserve\",\n"
              << "  \"darkBiasState\": \"" << photonstack::calibrationBiasStateName(darkBiasState) << "\",\n"
              << "  \"flatBiasState\": \"" << photonstack::calibrationBiasStateName(flatBiasState) << "\",\n"
              << "  \"biasSubtractedFromLight\": "
              << (result.subtractedBiasFromLight ? "true" : "false") << ",\n"
              << "  \"biasSubtractedFromFlat\": "
              << (result.subtractedBiasFromFlat ? "true" : "false") << ",\n"
              << "  \"rawWhiteBalance\": \"" << rawWhiteBalanceName(readOptions.raw.whiteBalanceMode) << "\",\n"
              << "  \"rawTemperature\": " << readOptions.raw.manualWhiteBalanceTemperature << ",\n"
              << "  \"rawTint\": " << readOptions.raw.manualWhiteBalanceTint << ",\n"
              << "  \"rawExposureBias\": " << readOptions.raw.exposureBias << ",\n"
              << "  \"rawBlackLevel\": \"" << rawBlackLevelName(readOptions.raw.blackLevelMode) << "\",\n"
              << "  \"rawBlackValue\": " << readOptions.raw.manualBlackLevel << ",\n"
              << "  \"rawDemosaic\": \"" << rawDemosaicName(readOptions.raw.demosaicQuality) << "\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int extractBackground(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::BackgroundExtractionOptions options;
    photonstack::BackgroundGridOptions gridOptions;
    std::string model = "global";

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--model") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--model requires global or grid");
                return 1;
            }
            model = args[++i];
            if (model != "global" && model != "grid") {
                printError("ArgumentInvalid", "--model must be global or grid");
                return 1;
            }
        } else if (arg == "--mode") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--mode requires subtract or divide");
                return 1;
            }
            const auto mode = args[++i];
            if (mode == "subtract") {
                options.mode = photonstack::BackgroundMode::Subtract;
            } else if (mode == "divide") {
                options.mode = photonstack::BackgroundMode::Divide;
            } else {
                printError("ArgumentInvalid", "--mode must be subtract or divide");
                return 1;
            }
        } else if (arg == "--strength") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--strength requires a number between 0 and 1");
                return 1;
            }
            try {
                options.strength = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--strength must be a number between 0 and 1");
                return 1;
            }
        } else if (arg == "--preserve-brightness") {
            if (!parseOnOff(args, i, "--preserve-brightness", options.preserveBrightness)) {
                return 1;
            }
        } else if (arg == "--protect-bright-targets") {
            if (!parseOnOff(args, i, "--protect-bright-targets", gridOptions.protectBrightTargets)) {
                return 1;
            }
        } else if (arg == "--grid-x") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--grid-x requires a positive integer");
                return 1;
            }
            try {
                gridOptions.columns = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--grid-x must be a positive integer");
                return 1;
            }
        } else if (arg == "--grid-y") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--grid-y requires a positive integer");
                return 1;
            }
            try {
                gridOptions.rows = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--grid-y must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown background argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "background requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "background requires --output <image>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    options.clampOutput = !scientificOutput;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::BackgroundExtractor extractor;
    gridOptions.extraction = options;
    const auto result = model == "grid" ? extractor.extractGrid(readResult.image, gridOptions)
                                        : extractor.extractGlobal(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"background\",\n"
              << "  \"model\": \"" << jsonEscape(model) << "\",\n"
              << "  \"mode\": \"" << (options.mode == photonstack::BackgroundMode::Divide ? "divide" : "subtract") << "\",\n"
              << "  \"strength\": " << options.strength << ",\n"
              << "  \"preserveBrightness\": " << (options.preserveBrightness ? "true" : "false") << ",\n"
              << "  \"protectBrightTargets\": " << (gridOptions.protectBrightTargets ? "true" : "false") << ",\n"
              << "  \"backgroundEstimator\": \""
              << (model == "grid" ? (gridOptions.protectBrightTargets ? "lower-quartile" : "median") : "global-median")
              << "\",\n"
              << "  \"sampledGridCells\": " << result.sampledGridCells << ",\n"
              << "  \"filledGridCells\": " << result.filledGridCells << ",\n"
              << "  \"divideNormalization\": \""
              << (options.mode == photonstack::BackgroundMode::Divide
                      ? (options.preserveBrightness ? "global-background" : "unity")
                      : "not-applicable")
              << "\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"clampOutput\": " << (options.clampOutput ? "true" : "false") << ",\n"
              << "  \"background\": " << result.background << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int printHistogram(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    photonstack::HistogramOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--bins") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--bins requires a positive integer");
                return 1;
            }
            try {
                options.bins = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--bins must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown histogram argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "histogram requires --input <image>");
        return 1;
    }

    photonstack::ImageReadOptions readOptions;
    readOptions.raw.linearOutput = false;
    readOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, readOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::Histogram histogram;
    const auto result = histogram.luminance(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"histogram\",\n"
              << "  \"minimum\": " << result.minimum << ",\n"
              << "  \"maximum\": " << result.maximum << ",\n"
              << "  \"mean\": " << result.mean << ",\n"
              << "  \"bins\": [";
    for (std::size_t i = 0; i < result.bins.size(); ++i) {
        if (i > 0) {
            std::cout << ", ";
        }
        std::cout << result.bins[i];
    }
    std::cout << "]\n"
              << "}\n";
    return 0;
}

int analyzeQuality(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    photonstack::FrameQualityOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                options.starDetection.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--min-peak requires a positive number");
                return 1;
            }
            try {
                options.starDetection.minPeak = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--min-peak must be a positive number");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown quality argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "quality requires --input <image>");
        return 1;
    }

    photonstack::ImageReadOptions readOptions;
    readOptions.raw.linearOutput = false;
    readOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, readOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::FrameQualityAnalyzer analyzer;
    const auto result = analyzer.analyze(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"quality\",\n"
              << "  \"valueDomain\": \"normalized-quality\",\n"
              << "  \"score\": " << result.score << ",\n"
              << "  \"starCount\": " << result.starCount << ",\n"
              << "  \"medianFwhm\": " << result.medianFwhm << ",\n"
              << "  \"medianEccentricity\": " << result.medianEccentricity << ",\n"
              << "  \"background\": " << result.background << ",\n"
              << "  \"noise\": " << result.noise << ",\n"
              << "  \"saturatedFraction\": " << result.saturatedFraction << "\n"
              << "}\n";
    return 0;
}

int normalizeImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> referencePath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::FrameNormalizationOptions options;
    photonstack::LocalNormalizationOptions localOptions;
    std::string model = "global";

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };

        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--reference") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--reference requires an image path");
                return 1;
            }
            referencePath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--target-background") {
            if (!parseFloat(options.targetBackground, "--target-background")) {
                return 1;
            }
        } else if (arg == "--target-scale") {
            if (!parseFloat(options.targetScale, "--target-scale")) {
                return 1;
            }
        } else if (arg == "--model") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--model requires global or local");
                return 1;
            }
            model = args[++i];
            if (model != "global" && model != "local") {
                printError("ArgumentInvalid", "--model must be global or local");
                return 1;
            }
        } else if (arg == "--grid-x") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--grid-x requires a positive integer");
                return 1;
            }
            try {
                localOptions.columns = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--grid-x must be a positive integer");
                return 1;
            }
        } else if (arg == "--grid-y") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--grid-y requires a positive integer");
                return 1;
            }
            try {
                localOptions.rows = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--grid-y must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown normalize argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "normalize requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "normalize requires --output <image>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    options.clampOutput = !scientificOutput;
    const auto readOptions = scientificFitsReadOptions();
    const photonstack::ImageCodec codec;
    const auto inputRead = codec.read(*inputPath, readOptions);
    if (!inputRead.ok) {
        printError(inputRead.errorCode, inputRead.message);
        return 1;
    }

    const photonstack::FrameNormalizer normalizer;
    photonstack::FrameNormalizationResult result;
    localOptions.normalization = options;
    if (referencePath.has_value()) {
        const auto referenceRead = codec.read(*referencePath, readOptions);
        if (!referenceRead.ok) {
            printError(referenceRead.errorCode, referenceRead.message);
            return 1;
        }
        result = model == "local" ? normalizer.matchReferenceLocal(inputRead.image, referenceRead.image, localOptions)
                                  : normalizer.matchReference(inputRead.image, referenceRead.image, options);
    } else if (model == "local") {
        printError("ArgumentMissing", "normalize --model local requires --reference <image>");
        return 1;
    } else {
        result = normalizer.normalize(inputRead.image, options);
    }
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"normalize\",\n"
              << "  \"model\": \"" << jsonEscape(model) << "\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"clampOutput\": " << (options.clampOutput ? "true" : "false") << ",\n"
              << "  \"inputBackground\": " << result.inputBackground << ",\n"
              << "  \"outputBackground\": " << result.outputBackground << ",\n"
              << "  \"scale\": " << result.scale << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int stretchImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::StretchOptions options;
    photonstack::AutoStretchOptions autoOptions;
    bool useAuto = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };

        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--auto") {
            useAuto = true;
        } else if (arg == "--black") {
            if (!parseFloat(options.blackPoint, "--black")) {
                return 1;
            }
        } else if (arg == "--mid") {
            if (!parseFloat(options.midPoint, "--mid")) {
                return 1;
            }
        } else if (arg == "--white") {
            if (!parseFloat(options.whitePoint, "--white")) {
                return 1;
            }
        } else if (arg == "--arcsinh") {
            if (!parseFloat(options.arcsinhStrength, "--arcsinh")) {
                return 1;
            }
            autoOptions.arcsinhStrength = options.arcsinhStrength;
        } else if (arg == "--target-background") {
            if (!parseFloat(autoOptions.targetBackground, "--target-background")) {
                return 1;
            }
        } else if (arg == "--shadows-sigma") {
            if (!parseFloat(autoOptions.shadowsSigma, "--shadows-sigma")) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown stretch argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "stretch requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "stretch requires --output <image>");
        return 1;
    }

    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::Stretch stretch;
    const auto result =
        useAuto ? stretch.applyAuto(readResult.image, autoOptions) : stretch.apply(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"stretch\",\n"
              << "  \"auto\": " << (useAuto ? "true" : "false") << ",\n"
              << "  \"inputValueDomain\": \"scientific\",\n"
              << "  \"outputValueDomain\": \"display-stretch\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

std::optional<photonstack::CurvesOptions> parseCurvePoints(const std::string& value) {
    photonstack::CurvesOptions options;
    options.points.clear();
    std::size_t start = 0;
    while (start < value.size()) {
        const auto comma = value.find(',', start);
        const auto token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const auto colon = token.find(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        try {
            options.points.push_back({
                .input = parseFiniteFloat(token.substr(0, colon)),
                .output = parseFiniteFloat(token.substr(colon + 1)),
            });
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return options;
}

std::optional<photonstack::CurveChannel> parseCurveChannel(const std::string& value) {
    if (value == "rgb") {
        return photonstack::CurveChannel::RGB;
    }
    if (value == "red") {
        return photonstack::CurveChannel::Red;
    }
    if (value == "green") {
        return photonstack::CurveChannel::Green;
    }
    if (value == "blue") {
        return photonstack::CurveChannel::Blue;
    }
    if (value == "luminance") {
        return photonstack::CurveChannel::Luminance;
    }
    return std::nullopt;
}

std::string curveChannelName(photonstack::CurveChannel channel) {
    switch (channel) {
    case photonstack::CurveChannel::RGB:
        return "rgb";
    case photonstack::CurveChannel::Red:
        return "red";
    case photonstack::CurveChannel::Green:
        return "green";
    case photonstack::CurveChannel::Blue:
        return "blue";
    case photonstack::CurveChannel::Luminance:
        return "luminance";
    }
    return "rgb";
}

int applyCurves(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::CurvesOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--points") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--points requires input:output pairs");
                return 1;
            }
            const auto parsed = parseCurvePoints(args[++i]);
            if (!parsed.has_value()) {
                printError("ArgumentInvalid", "--points must look like 0:0,0.5:0.6,1:1");
                return 1;
            }
            options.points = parsed->points;
        } else if (arg == "--channel") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--channel requires rgb, red, green, blue, or luminance");
                return 1;
            }
            const auto parsed = parseCurveChannel(args[++i]);
            if (!parsed.has_value()) {
                printError("ArgumentInvalid", "--channel must be rgb, red, green, blue, or luminance");
                return 1;
            }
            options.channel = *parsed;
        } else {
            printError("ArgumentInvalid", "Unknown curves argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "curves requires --input <image> and --output <image>");
        return 1;
    }

    photonstack::ImageReadOptions displayReadOptions;
    displayReadOptions.raw.linearOutput = false;
    displayReadOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto displayRead = codec.read(*inputPath, displayReadOptions);
    if (!displayRead.ok) {
        printError(displayRead.errorCode, displayRead.message);
        return 1;
    }
    const photonstack::Curves curves;
    const auto result = curves.apply(displayRead.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    photonstack::ImageBuffer output = result.image;
    const bool scientificBridge = photonstack::isFitsPath(*inputPath) && photonstack::isFitsPath(*outputPath);
    if (scientificBridge) {
        const auto scientificRead = codec.read(*inputPath, scientificFitsReadOptions());
        if (!scientificRead.ok) {
            printError(scientificRead.errorCode, scientificRead.message);
            return 1;
        }
        const photonstack::ScientificDisplayBridge bridge;
        const auto bridgeResult = bridge.applyCorrections(scientificRead.image, displayRead.image, result.image);
        if (!bridgeResult.ok) {
            printError(bridgeResult.errorCode, bridgeResult.message);
            return 1;
        }
        output = bridgeResult.image;
    }

    const auto writeResult = codec.write(output, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"curves\",\n"
              << "  \"channel\": \"" << curveChannelName(options.channel) << "\",\n"
              << "  \"valueDomain\": \"" << (scientificBridge ? "scientific-display-bridge" : "display")
              << "\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int applyLocalContrast(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::LocalContrastOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--amount") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--amount requires a number");
                return 1;
            }
            try {
                options.amount = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--amount must be a number");
                return 1;
            }
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown local-contrast argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "local-contrast requires --input <image> and --output <image>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    options.clampOutput = !scientificOutput;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }
    const photonstack::LocalContrast localContrast;
    const auto result = localContrast.apply(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }
    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"local-contrast\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"clampOutput\": " << (options.clampOutput ? "true" : "false") << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int denoiseImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::NoiseReductionOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--amount") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--amount requires a number between 0 and 1");
                return 1;
            }
            try {
                options.amount = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--amount must be a number between 0 and 1");
                return 1;
            }
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--chroma-amount") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--chroma-amount requires a number between 0 and 1");
                return 1;
            }
            try {
                options.chromaAmount = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--chroma-amount must be a number between 0 and 1");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown denoise argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "denoise requires --input <image> and --output <image>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    options.clampOutput = !scientificOutput;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }
    const photonstack::NoiseReducer reducer;
    const auto result = reducer.reduce(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }
    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"denoise\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"clampOutput\": " << (options.clampOutput ? "true" : "false") << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int sharpenImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::SharpenOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--amount") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--amount requires a number");
                return 1;
            }
            try {
                options.amount = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--amount must be a number");
                return 1;
            }
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown sharpen argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "sharpen requires --input <image> and --output <image>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    options.clampOutput = !scientificOutput;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }
    const photonstack::Sharpen sharpen;
    const auto result = sharpen.unsharpMask(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }
    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"sharpen\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"clampOutput\": " << (options.clampOutput ? "true" : "false") << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int deconvolveImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::DeconvolutionOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--iterations") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--iterations requires a positive integer");
                return 1;
            }
            try {
                options.iterations = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--iterations must be a positive integer");
                return 1;
            }
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--sigma") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma requires a positive number");
                return 1;
            }
            try {
                options.sigma = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma must be a positive number");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown deconvolve argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "deconvolve requires --input <image> and --output <image>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    options.clampOutput = !scientificOutput;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::Deconvolution deconvolution;
    const auto result = deconvolution.richardsonLucy(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"deconvolve\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"clampOutput\": " << (options.clampOutput ? "true" : "false") << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int adjustColor(const std::string& mode, const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::BackgroundNeutralizationOptions neutralizeOptions;
    photonstack::SaturationOptions saturationOptions;
    photonstack::GreenCastSuppressionOptions greenOptions;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--strength" && mode == "neutralize") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--strength requires a number between 0 and 1");
                return 1;
            }
            try {
                neutralizeOptions.strength = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--strength must be a number between 0 and 1");
                return 1;
            }
        } else if (arg == "--amount" && mode == "saturate") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--amount requires a number");
                return 1;
            }
            try {
                saturationOptions.amount = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--amount must be a number");
                return 1;
            }
        } else if (arg == "--amount" && mode == "remove-green") {
            if (!parseFloat(greenOptions.amount, "--amount")) {
                return 1;
            }
        } else if (arg == "--background-limit" && mode == "remove-green") {
            if (!parseFloat(greenOptions.backgroundLimit, "--background-limit")) {
                return 1;
            }
        } else if (arg == "--green-threshold" && mode == "remove-green") {
            if (!parseFloat(greenOptions.greenExcessThreshold, "--green-threshold")) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown color argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "color " + mode + " requires --input <image> and --output <image>");
        return 1;
    }

    photonstack::ImageReadOptions displayReadOptions;
    displayReadOptions.raw.linearOutput = false;
    displayReadOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto displayRead = codec.read(*inputPath, displayReadOptions);
    if (!displayRead.ok) {
        printError(displayRead.errorCode, displayRead.message);
        return 1;
    }

    const photonstack::ColorAdjuster adjuster;
    const auto result =
        mode == "neutralize" ? adjuster.neutralizeBackground(displayRead.image, neutralizeOptions)
                             : (mode == "remove-green" ? adjuster.suppressGreenCast(displayRead.image, greenOptions)
                                                       : adjuster.adjustSaturation(displayRead.image, saturationOptions));
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    photonstack::ImageBuffer output = result.image;
    const bool scientificBridge = photonstack::isFitsPath(*inputPath) && photonstack::isFitsPath(*outputPath);
    if (scientificBridge) {
        const auto scientificRead = codec.read(*inputPath, scientificFitsReadOptions());
        if (!scientificRead.ok) {
            printError(scientificRead.errorCode, scientificRead.message);
            return 1;
        }
        const photonstack::ScientificDisplayBridge bridge;
        const auto bridgeResult = bridge.applyCorrections(scientificRead.image, displayRead.image, result.image);
        if (!bridgeResult.ok) {
            printError(bridgeResult.errorCode, bridgeResult.message);
            return 1;
        }
        output = bridgeResult.image;
    }

    const auto writeResult = codec.write(output, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"color " << jsonEscape(mode) << "\",\n"
              << "  \"affectedPixels\": " << result.affectedPixels << ",\n"
              << "  \"valueDomain\": \"" << (scientificBridge ? "scientific-display-bridge" : "display")
              << "\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int buildMasterFrame(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> outputPath;
    photonstack::MasterFrameOptions options;
    std::vector<std::filesystem::path> inputFiles;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--method") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--method requires median or average");
                return 1;
            }
            const auto method = args[++i];
            if (method == "median") {
                options.method = photonstack::MasterFrameMethod::Median;
            } else if (method == "average") {
                options.method = photonstack::MasterFrameMethod::Average;
            } else {
                printError("ArgumentInvalid", "--method must be median or average");
                return 1;
            }
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, options.raw)) {
                return 1;
            }
        } else {
            inputFiles.emplace_back(arg);
        }
    }

    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "master requires --output <image>");
        return 1;
    }
    if (inputFiles.empty()) {
        printError("InputMissing", "master requires at least one input image");
        return 1;
    }
    if (!options.raw.linearOutput) {
        printError("ArgumentInvalid", "master requires --raw-linear on");
        return 1;
    }

    const photonstack::MasterFrameBuilder builder;
    const auto result = builder.build(inputFiles, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const photonstack::ImageCodec codec;
    const auto writeResult = codec.write(result.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"master\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"rawWhiteBalance\": \"" << rawWhiteBalanceName(options.raw.whiteBalanceMode) << "\",\n"
              << "  \"rawTemperature\": " << options.raw.manualWhiteBalanceTemperature << ",\n"
              << "  \"rawTint\": " << options.raw.manualWhiteBalanceTint << ",\n"
              << "  \"rawExposureBias\": " << options.raw.exposureBias << ",\n"
              << "  \"rawBlackLevel\": \"" << rawBlackLevelName(options.raw.blackLevelMode) << "\",\n"
              << "  \"rawBlackValue\": " << options.raw.manualBlackLevel << ",\n"
              << "  \"rawDemosaic\": \"" << rawDemosaicName(options.raw.demosaicQuality) << "\",\n"
              << "  \"frames\": " << inputFiles.size() << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int buildMosaic(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> outputPath;
    photonstack::MosaicOptions options;
    std::vector<std::filesystem::path> inputFiles;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--overlap-pixels") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--overlap-pixels requires a non-negative integer");
                return 1;
            }
            try {
                options.overlapPixels = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--overlap-pixels must be a non-negative integer");
                return 1;
            }
        } else if (arg == "--projection") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--projection requires planar or cylindrical");
                return 1;
            }
            const auto projection = args[++i];
            if (projection == "planar") {
                options.projection = photonstack::MosaicProjection::Planar;
            } else if (projection == "cylindrical") {
                options.projection = photonstack::MosaicProjection::Cylindrical;
            } else {
                printError("ArgumentInvalid", "--projection must be planar or cylindrical");
                return 1;
            }
        } else if (arg == "--layout") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--layout requires horizontal or grid");
                return 1;
            }
            const auto layout = args[++i];
            if (layout == "horizontal") {
                options.layout = photonstack::MosaicLayout::Horizontal;
            } else if (layout == "grid") {
                options.layout = photonstack::MosaicLayout::Grid;
            } else {
                printError("ArgumentInvalid", "--layout must be horizontal or grid");
                return 1;
            }
        } else if (arg == "--columns") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--columns requires a positive integer");
                return 1;
            }
            try {
                options.columns = std::max<std::uint32_t>(1U, parseUnsignedInteger<std::uint32_t>(args[++i]));
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--columns must be a positive integer");
                return 1;
            }
        } else if (arg == "--alignment") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--alignment requires manual or auto");
                return 1;
            }
            const auto alignment = args[++i];
            if (alignment == "manual") {
                options.alignment = photonstack::MosaicAlignment::Manual;
            } else if (alignment == "auto") {
                options.alignment = photonstack::MosaicAlignment::Auto;
                options.registration.matchTolerance = 4.0F;
                options.registration.starDetection.sigmaThreshold = 2.0F;
            } else {
                printError("ArgumentInvalid", "--alignment must be manual or auto");
                return 1;
            }
        } else if (arg == "--blend") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--blend requires average, feather, or multiband");
                return 1;
            }
            const auto blend = args[++i];
            if (blend == "average") {
                options.blendMode = photonstack::MosaicBlendMode::Average;
            } else if (blend == "feather") {
                options.blendMode = photonstack::MosaicBlendMode::Feather;
            } else if (blend == "multiband") {
                options.blendMode = photonstack::MosaicBlendMode::Multiband;
            } else {
                printError("ArgumentInvalid", "--blend must be average, feather, or multiband");
                return 1;
            }
        } else if (arg == "--exposure-match") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--exposure-match requires on or off");
                return 1;
            }
            const auto value = args[++i];
            if (value == "on") {
                options.exposureMatching = true;
            } else if (value == "off") {
                options.exposureMatching = false;
            } else {
                printError("ArgumentInvalid", "--exposure-match must be on or off");
                return 1;
            }
        } else if (arg == "--preview-width") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--preview-width requires a positive integer");
                return 1;
            }
            try {
                options.previewWidth = std::max<std::uint32_t>(1U, parseUnsignedInteger<std::uint32_t>(args[++i]));
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--preview-width must be a positive integer");
                return 1;
            }
        } else if (arg == "--alignment-width") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--alignment-width requires a positive integer");
                return 1;
            }
            try {
                options.alignmentWidth = std::max<std::uint32_t>(1U, parseUnsignedInteger<std::uint32_t>(args[++i]));
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--alignment-width must be a positive integer");
                return 1;
            }
        } else {
            inputFiles.emplace_back(arg);
        }
    }

    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "mosaic requires --output <image>");
        return 1;
    }
    if (inputFiles.empty()) {
        printError("InputMissing", "mosaic requires at least one input image");
        return 1;
    }

    std::optional<photonstack::MosaicProgressStage> lastMosaicStage;
    double lastMosaicProgress = -1.0;
    options.progress = [&](const photonstack::MosaicProgress& event) {
        const char* stage = "read";
        switch (event.stage) {
        case photonstack::MosaicProgressStage::Reading:
            stage = "read";
            break;
        case photonstack::MosaicProgressStage::Resizing:
            stage = "resize";
            break;
        case photonstack::MosaicProgressStage::Projecting:
            stage = "project";
            break;
        case photonstack::MosaicProgressStage::Aligning:
            stage = "align";
            break;
        case photonstack::MosaicProgressStage::ExposureMatching:
            stage = "exposure";
            break;
        case photonstack::MosaicProgressStage::Blending:
            stage = "blend";
            break;
        case photonstack::MosaicProgressStage::Normalizing:
            stage = "normalize";
            break;
        }
        const double mappedProgress = 0.02 + event.progress * 0.88;
        const bool stageChanged = !lastMosaicStage.has_value() || *lastMosaicStage != event.stage;
        if (!stageChanged && event.progress < 1.0 && mappedProgress - lastMosaicProgress < 0.0025) {
            return;
        }
        lastMosaicStage = event.stage;
        lastMosaicProgress = mappedProgress;
        const auto step = event.panel > 0 ? std::optional<std::size_t>(event.panel) : std::nullopt;
        const auto total = event.panel > 0 ? std::optional<std::size_t>(event.panelCount) : std::nullopt;
        printProgressEvent("mosaic", "mosaic", stage, mappedProgress, step, total);
    };

    const photonstack::MosaicBuilder builder;
    const auto result = builder.stitchHorizontal(inputFiles, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }
    const photonstack::ImageCodec codec;
    photonstack::ImageWriteResult writeResult;
    printProgressEvent("mosaic", "mosaic", "write", 0.91);
    if (result.image.channels == 4) {
        writeResult = codec.writeRows(result.image, *outputPath, [&](const photonstack::ImageRowConsumer& consumer) {
            const auto rowSamples = static_cast<std::size_t>(result.image.width) * result.image.channels;
            const auto interval = std::max<std::uint32_t>(1, result.image.height / 100);
            for (std::uint32_t row = 0; row < result.image.height; ++row) {
                const auto offset = static_cast<std::size_t>(row) * rowSamples;
                if (!consumer(row, result.image.pixels.data() + offset, rowSamples)) {
                    return false;
                }
                const auto completed = row + 1;
                if (completed == result.image.height || completed % interval == 0) {
                    const double fraction = static_cast<double>(completed) / result.image.height;
                    printProgressEvent("mosaic", "mosaic", "write", 0.91 + fraction * 0.08,
                                       completed, result.image.height);
                }
            }
            return true;
        });
    }
    if (result.image.channels != 4 ||
        (!writeResult.ok && writeResult.errorCode == "ImageRowWriteUnsupported")) {
        writeResult = codec.write(result.image, *outputPath);
        if (writeResult.ok) {
            printProgressEvent("mosaic", "mosaic", "write", 0.99);
        }
    }
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    const auto transformModelName = [](photonstack::MosaicTransformModel model) {
        switch (model) {
        case photonstack::MosaicTransformModel::Translation:
            return "translation";
        case photonstack::MosaicTransformModel::Similarity:
            return "similarity";
        case photonstack::MosaicTransformModel::Affine:
            return "affine";
        case photonstack::MosaicTransformModel::Manual:
        default:
            return "manual";
        }
    };

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"mosaic\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"frames\": " << inputFiles.size() << ",\n"
              << "  \"width\": " << result.image.width << ",\n"
              << "  \"height\": " << result.image.height << ",\n"
              << "  \"autoAligned\": " << (result.usedAutoAlignment ? "true" : "false") << ",\n"
              << "  \"matches\": " << result.matchedPairs << ",\n"
              << "  \"fallbackPanels\": " << result.fallbackPanels << ",\n"
              << "  \"exposureMatched\": " << (result.exposureMatched ? "true" : "false") << ",\n"
              << "  \"blend\": \"";
    if (result.blendMode == photonstack::MosaicBlendMode::Multiband) {
        std::cout << "multiband";
    } else {
        std::cout << (result.blendMode == photonstack::MosaicBlendMode::Feather ? "feather" : "average");
    }
    std::cout << "\",\n"
              << "  \"placements\": [";
    for (std::size_t i = 0; i < result.placements.size(); ++i) {
        const auto& placement = result.placements[i];
        if (i > 0) {
            std::cout << ", ";
        }
        std::cout << "{\"index\":" << i
                  << ",\"x\":" << placement.x
                  << ",\"y\":" << placement.y
                  << ",\"a\":" << placement.a
                  << ",\"b\":" << placement.b
                  << ",\"c\":" << placement.c
                  << ",\"d\":" << placement.d
                  << ",\"dx\":" << placement.dx
                  << ",\"dy\":" << placement.dy
                  << ",\"scaleX\":" << std::hypot(placement.a, placement.c)
                  << ",\"scaleY\":" << std::hypot(placement.b, placement.d)
                  << ",\"rotationDegrees\":" << std::atan2(placement.c, placement.a) * 180.0 / 3.14159265358979323846
                  << ",\"model\":\"" << transformModelName(placement.transformModel) << "\""
                  << ",\"matches\":" << placement.matches
                  << ",\"references\":" << placement.referenceCount
                  << ",\"autoAligned\":" << (placement.autoAligned ? "true" : "false")
                  << ",\"fallback\":" << (placement.usedFallback ? "true" : "false")
                  << ",\"reducedModel\":" << (placement.usedReducedModel ? "true" : "false")
                  << ",\"coarseAlignment\":" << (placement.usedCoarseAlignment ? "true" : "false")
                  << "}";
    }
    std::cout << "],\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

void printVersion() {
    std::cout << "PhotonStack " << photonstack::kPhotonStackVersion << '\n'
              << "Engine " << photonstack::kPhotonStackEngineVersion << '\n'
              << "Commit " << photonstack::kPhotonStackGitCommit << '\n'
              << "Build " << photonstack::kPhotonStackBuildDate << '\n';
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += c;
            break;
        }
    }
    return escaped;
}

bool progressEventsEnabled() {
    const char* value = std::getenv("PHOTONSTACK_PROGRESS");
    return value != nullptr && std::string(value) == "1";
}

std::string commandLabel(const std::vector<std::string>& args) {
    if (args.empty()) {
        return "unknown";
    }
    if ((args.front() == "color" || args.front() == "stars" || args.front() == "fits" || args.front() == "raw") &&
        args.size() > 1) {
        return args.front() + " " + args[1];
    }
    return args.front();
}

void printProgressEvent(
    const std::string& task,
    const std::string& command,
    const std::string& stage,
    double progress,
    std::optional<std::size_t> step,
    std::optional<std::size_t> total
) {
    if (!progressEventsEnabled()) {
        return;
    }

    const double clampedProgress = std::clamp(progress, 0.0, 1.0);
    std::cout << "{\"type\":\"progress\",\"task\":\"" << jsonEscape(task)
              << "\",\"command\":\"" << jsonEscape(command)
              << "\",\"stage\":\"" << jsonEscape(stage)
              << "\",\"progress\":" << clampedProgress;
    if (step.has_value()) {
        std::cout << ",\"step\":" << step.value();
    }
    if (total.has_value()) {
        std::cout << ",\"total\":" << total.value();
    }
    std::cout << "}\n";
    std::cout.flush();
}

int inspectImage(const std::string& path) {
    const photonstack::ImageInspector inspector;
    const auto result = inspector.inspect(path);

    if (!result.ok) {
        std::cerr << "{\n"
                  << "  \"type\": \"error\",\n"
                  << "  \"code\": \"" << jsonEscape(result.errorCode) << "\",\n"
                  << "  \"message\": \"" << jsonEscape(result.message) << "\",\n"
                  << "  \"path\": \"" << jsonEscape(result.metadata.path) << "\"\n"
                  << "}\n";
        return 1;
    }

    const auto& metadata = result.metadata;
    auto writeStringField = [](bool& first, const char* key, const std::string& value) {
        if (value.empty()) {
            return;
        }
        std::cout << (first ? "" : ",\n")
                  << "    \"" << key << "\": \"" << jsonEscape(value) << "\"";
        first = false;
    };
    auto writeUIntField = [](bool& first, const char* key, std::uint32_t value) {
        if (value == 0) {
            return;
        }
        std::cout << (first ? "" : ",\n")
                  << "    \"" << key << "\": " << value;
        first = false;
    };
    auto writeDoubleField = [](bool& first, const char* key, double value) {
        if (value <= 0.0) {
            return;
        }
        std::cout << (first ? "" : ",\n")
                  << "    \"" << key << "\": " << value;
        first = false;
    };

    std::cout << "{\n"
              << "  \"path\": \"" << jsonEscape(metadata.path) << "\",\n"
              << "  \"format\": \"" << photonstack::toString(metadata.format) << "\",\n"
              << "  \"width\": " << metadata.width << ",\n"
              << "  \"height\": " << metadata.height << ",\n"
              << "  \"channels\": " << metadata.channels << ",\n"
              << "  \"bitsPerChannel\": " << metadata.bitsPerChannel << ",\n"
              << "  \"metadata\": {\n";
    bool firstMetadataField = true;
    writeStringField(firstMetadataField, "cameraMake", metadata.cameraMake);
    writeStringField(firstMetadataField, "cameraModel", metadata.cameraModel);
    writeStringField(firstMetadataField, "lensModel", metadata.lensModel);
    writeStringField(firstMetadataField, "captureDate", metadata.captureDate);
    writeStringField(firstMetadataField, "colorModel", metadata.colorModel);
    writeStringField(firstMetadataField, "colorProfile", metadata.colorProfile);
    writeStringField(firstMetadataField, "rawDecoder", metadata.rawDecoder);
    writeStringField(firstMetadataField, "whiteBalance", metadata.whiteBalance);
    writeStringField(firstMetadataField, "exposureBias", metadata.exposureBias);
    writeDoubleField(firstMetadataField, "exposureTimeSeconds", metadata.exposureTimeSeconds);
    writeDoubleField(firstMetadataField, "fNumber", metadata.fNumber);
    writeDoubleField(firstMetadataField, "focalLengthMM", metadata.focalLengthMM);
    writeUIntField(firstMetadataField, "iso", metadata.iso);
    writeUIntField(firstMetadataField, "orientation", metadata.orientation);
    if (!firstMetadataField) {
        std::cout << "\n";
    }
    std::cout << "  }\n"
              << "}\n";
    return 0;
}

std::string rawWhiteBalanceName(photonstack::RawWhiteBalanceMode mode) {
    switch (mode) {
    case photonstack::RawWhiteBalanceMode::Auto:
        return "auto";
    case photonstack::RawWhiteBalanceMode::Daylight:
        return "daylight";
    case photonstack::RawWhiteBalanceMode::Manual:
        return "manual";
    case photonstack::RawWhiteBalanceMode::Camera:
    default:
        return "camera";
    }
}

std::string rawBlackLevelName(photonstack::RawBlackLevelMode mode) {
    switch (mode) {
    case photonstack::RawBlackLevelMode::Auto:
        return "auto";
    case photonstack::RawBlackLevelMode::Manual:
        return "manual";
    case photonstack::RawBlackLevelMode::Camera:
    default:
        return "camera";
    }
}

std::string rawDemosaicName(photonstack::RawDemosaicQuality quality) {
    switch (quality) {
    case photonstack::RawDemosaicQuality::Fast:
        return "fast";
    case photonstack::RawDemosaicQuality::High:
        return "high";
    case photonstack::RawDemosaicQuality::Balanced:
    default:
        return "balanced";
    }
}

bool parseRawDecodeArgument(
    const std::vector<std::string>& args,
    std::size_t& index,
    photonstack::RawDecodeOptions& options
) {
    const auto& arg = args[index];
    if (arg == "--raw-white-balance") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-white-balance requires camera, auto, daylight, or manual");
            return false;
        }
        const auto value = args[++index];
        if (value == "camera") {
            options.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Camera;
        } else if (value == "auto") {
            options.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Auto;
        } else if (value == "daylight") {
            options.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Daylight;
        } else if (value == "manual") {
            options.whiteBalanceMode = photonstack::RawWhiteBalanceMode::Manual;
        } else {
            printError("ArgumentInvalid", "--raw-white-balance must be camera, auto, daylight, or manual");
            return false;
        }
        return true;
    }
    if (arg == "--raw-exposure-bias") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-exposure-bias requires a stop value");
            return false;
        }
        try {
            options.exposureBias = parseFiniteFloat(args[++index]);
        } catch (const std::exception&) {
            printError("ArgumentInvalid", "--raw-exposure-bias must be a number");
            return false;
        }
        if (options.exposureBias < -5.0F || options.exposureBias > 5.0F) {
            printError("ArgumentInvalid", "--raw-exposure-bias must be between -5 and 5 stops");
            return false;
        }
        return true;
    }
    if (arg == "--raw-temperature") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-temperature requires a Kelvin value");
            return false;
        }
        try {
            options.manualWhiteBalanceTemperature = parseFiniteFloat(args[++index]);
        } catch (const std::exception&) {
            printError("ArgumentInvalid", "--raw-temperature must be a number");
            return false;
        }
        if (options.manualWhiteBalanceTemperature < 2000.0F ||
            options.manualWhiteBalanceTemperature > 50000.0F) {
            printError("ArgumentInvalid", "--raw-temperature must be between 2000 and 50000 Kelvin");
            return false;
        }
        return true;
    }
    if (arg == "--raw-tint") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-tint requires a value");
            return false;
        }
        try {
            options.manualWhiteBalanceTint = parseFiniteFloat(args[++index]);
        } catch (const std::exception&) {
            printError("ArgumentInvalid", "--raw-tint must be a number");
            return false;
        }
        if (options.manualWhiteBalanceTint < -150.0F || options.manualWhiteBalanceTint > 150.0F) {
            printError("ArgumentInvalid", "--raw-tint must be between -150 and 150");
            return false;
        }
        return true;
    }
    if (arg == "--raw-black-value") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-black-value requires a normalized value");
            return false;
        }
        try {
            options.manualBlackLevel = parseFiniteFloat(args[++index]);
        } catch (const std::exception&) {
            printError("ArgumentInvalid", "--raw-black-value must be a number");
            return false;
        }
        if (options.manualBlackLevel < 0.0F || options.manualBlackLevel > 1.0F) {
            printError("ArgumentInvalid", "--raw-black-value must be between 0 and 1");
            return false;
        }
        return true;
    }
    if (arg == "--raw-black-level") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-black-level requires camera, auto, or manual");
            return false;
        }
        const auto value = args[++index];
        if (value == "camera") {
            options.blackLevelMode = photonstack::RawBlackLevelMode::Camera;
        } else if (value == "auto") {
            options.blackLevelMode = photonstack::RawBlackLevelMode::Auto;
        } else if (value == "manual") {
            options.blackLevelMode = photonstack::RawBlackLevelMode::Manual;
        } else {
            printError("ArgumentInvalid", "--raw-black-level must be camera, auto, or manual");
            return false;
        }
        return true;
    }
    if (arg == "--raw-demosaic") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-demosaic requires fast or high");
            return false;
        }
        const auto value = args[++index];
        if (value == "fast") {
            options.demosaicQuality = photonstack::RawDemosaicQuality::Fast;
        } else if (value == "balanced") {
            options.demosaicQuality = photonstack::RawDemosaicQuality::Balanced;
        } else if (value == "high") {
            options.demosaicQuality = photonstack::RawDemosaicQuality::High;
        } else {
            printError("ArgumentInvalid", "--raw-demosaic must be fast or high (balanced is a legacy alias)");
            return false;
        }
        return true;
    }
    if (arg == "--raw-linear") {
        if (index + 1 >= args.size()) {
            printError("ArgumentMissing", "--raw-linear requires on or off");
            return false;
        }
        const auto value = args[++index];
        if (value == "on" || value == "true" || value == "1") {
            options.linearOutput = true;
        } else if (value == "off" || value == "false" || value == "0") {
            options.linearOutput = false;
        } else {
            printError("ArgumentInvalid", "--raw-linear must be on or off");
            return false;
        }
        return true;
    }
    return false;
}

int convertImage(const std::vector<std::string>& args, const std::string& commandName = "convert") {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::ImageReadOptions readOptions;
    photonstack::ImageWriteOptions writeOptions;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--bit-depth") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--bit-depth requires auto, 8, or 16");
                return 1;
            }
            const auto value = args[++i];
            if (value == "auto") {
                writeOptions.bitDepth = photonstack::ImageWriteBitDepth::Auto;
            } else if (value == "8") {
                writeOptions.bitDepth = photonstack::ImageWriteBitDepth::Eight;
            } else if (value == "16") {
                writeOptions.bitDepth = photonstack::ImageWriteBitDepth::Sixteen;
            } else {
                printError("ArgumentInvalid", "--bit-depth must be auto, 8, or 16");
                return 1;
            }
        } else if (arg == "--color-space") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--color-space requires srgb or linear-srgb");
                return 1;
            }
            const auto value = args[++i];
            if (value == "srgb") {
                writeOptions.colorSpace = photonstack::ImageWriteColorSpace::SRGB;
            } else if (value == "linear-srgb") {
                writeOptions.colorSpace = photonstack::ImageWriteColorSpace::LinearSRGB;
            } else {
                printError("ArgumentInvalid", "--color-space must be srgb or linear-srgb");
                return 1;
            }
        } else if (arg == "--quality") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--quality requires a number from 0 to 1");
                return 1;
            }
            try {
                writeOptions.quality = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--quality must be a number from 0 to 1");
                return 1;
            }
            if (writeOptions.quality < 0.0F || writeOptions.quality > 1.0F) {
                printError("ArgumentInvalid", "--quality must be between 0 and 1");
                return 1;
            }
        } else if (arg == "--fits-values") {
            if (!parseFitsValuesArgument(args, i, readOptions.fits)) {
                return 1;
            }
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, readOptions.raw)) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown convert argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", commandName + " requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", commandName + " requires --output <image>");
        return 1;
    }
    if (!photonstack::isSupportedImageWritePath(*outputPath)) {
        printError("OutputFormatUnsupported", "Output image extension must be PNG, JPEG, HEIF, TIFF, or FITS");
        return 1;
    }
    readOptions.fits.maskNonFinitePixels = !photonstack::isFitsPath(*outputPath);

    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, readOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const auto writeResult = codec.write(readResult.image, *outputPath, writeOptions);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    const bool fitsOutput = photonstack::isFitsPath(*outputPath);
    const auto bitDepthName = [&writeOptions, &outputPath, fitsOutput]() {
        if (fitsOutput) {
            return std::string("32");
        }
        switch (photonstack::effectiveImageWriteBitDepth(*outputPath, writeOptions)) {
        case photonstack::ImageWriteBitDepth::Auto:
            return std::string("auto");
        case photonstack::ImageWriteBitDepth::Eight:
            return std::string("8");
        case photonstack::ImageWriteBitDepth::Sixteen:
            return std::string("16");
        }
        return std::string("auto");
    }();
    const auto colorSpaceName = fitsOutput || writeOptions.colorSpace == photonstack::ImageWriteColorSpace::LinearSRGB
                                    ? "linear-srgb"
                                    : "srgb";
    const auto sampleTypeName = fitsOutput ? "float" : "unsigned-integer";
    const auto nonFiniteHandlingName = readOptions.fits.mode == photonstack::FitsDecodeMode::DisplayNormalized
                                           ? "display-zero"
                                           : (readOptions.fits.maskNonFinitePixels ? "mask" : "preserve");
    auto outputExtension = outputPath->extension().string();
    std::transform(outputExtension.begin(), outputExtension.end(), outputExtension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto alphaHandlingName = outputExtension == ".jpg" || outputExtension == ".jpeg"
                                       ? "flatten-black"
                                       : "preserve";

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"" << jsonEscape(commandName) << "\",\n"
              << "  \"width\": " << readResult.image.width << ",\n"
              << "  \"height\": " << readResult.image.height << ",\n"
              << "  \"decodeBackend\": \"" << photonstack::imageReadBackendName(readResult.backend) << "\",\n"
              << "  \"decodeFallback\": " << (readResult.usedFallback ? "true" : "false") << ",\n"
              << "  \"decodeFallbackCode\": \"" << jsonEscape(readResult.fallbackErrorCode) << "\",\n"
              << "  \"decodeFallbackMessage\": \"" << jsonEscape(readResult.fallbackMessage) << "\",\n"
              << "  \"bitDepth\": \"" << bitDepthName << "\",\n"
              << "  \"sampleType\": \"" << sampleTypeName << "\",\n"
              << "  \"colorSpace\": \"" << colorSpaceName << "\",\n"
              << "  \"alphaHandling\": \"" << alphaHandlingName << "\",\n"
              << "  \"fitsValues\": \"" << fitsDecodeModeName(readOptions.fits.mode) << "\",\n"
              << "  \"nonFiniteHandling\": \"" << nonFiniteHandlingName << "\",\n"
              << "  \"quality\": " << writeOptions.quality << ",\n"
              << "  \"rawWhiteBalance\": \"" << rawWhiteBalanceName(readOptions.raw.whiteBalanceMode) << "\",\n"
              << "  \"rawTemperature\": " << readOptions.raw.manualWhiteBalanceTemperature << ",\n"
              << "  \"rawTint\": " << readOptions.raw.manualWhiteBalanceTint << ",\n"
              << "  \"rawExposureBias\": " << readOptions.raw.exposureBias << ",\n"
              << "  \"rawBlackLevel\": \"" << rawBlackLevelName(readOptions.raw.blackLevelMode) << "\",\n"
              << "  \"rawBlackValue\": " << readOptions.raw.manualBlackLevel << ",\n"
              << "  \"rawDemosaic\": \"" << rawDemosaicName(readOptions.raw.demosaicQuality) << "\",\n"
              << "  \"rawLinear\": " << (readOptions.raw.linearOutput ? "true" : "false") << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int adjustRawPreviewImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::RawDecodeOptions rawOptions;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, rawOptions)) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown raw adjust argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "raw adjust requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "raw adjust requires --output <image>");
        return 1;
    }

    const photonstack::ImageCodec codec;
    auto readResult = codec.read(*inputPath);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    photonstack::applyRawDecodeOptions(readResult.image, rawOptions);
    const auto writeResult = codec.write(readResult.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"raw adjust\",\n"
              << "  \"width\": " << readResult.image.width << ",\n"
              << "  \"height\": " << readResult.image.height << ",\n"
              << "  \"rawWhiteBalance\": \"" << rawWhiteBalanceName(rawOptions.whiteBalanceMode) << "\",\n"
              << "  \"rawTemperature\": " << rawOptions.manualWhiteBalanceTemperature << ",\n"
              << "  \"rawTint\": " << rawOptions.manualWhiteBalanceTint << ",\n"
              << "  \"rawExposureBias\": " << rawOptions.exposureBias << ",\n"
              << "  \"rawBlackLevel\": \"" << rawBlackLevelName(rawOptions.blackLevelMode) << "\",\n"
              << "  \"rawBlackValue\": " << rawOptions.manualBlackLevel << ",\n"
              << "  \"rawDemosaic\": \"" << rawDemosaicName(rawOptions.demosaicQuality) << "\",\n"
              << "  \"rawLinear\": " << (rawOptions.linearOutput ? "true" : "false") << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

void printError(const std::string& code, const std::string& message) {
    std::cerr << "{\n"
              << "  \"type\": \"error\",\n"
              << "  \"code\": \"" << jsonEscape(code) << "\",\n"
              << "  \"message\": \"" << jsonEscape(message) << "\"\n"
              << "}\n";
}

bool isSupportedImagePath(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".heic" ||
           extension == ".heif" || extension == ".hif" || extension == ".tif" || extension == ".tiff" ||
           photonstack::isFitsPath(path) || photonstack::isRawPath(path);
}

std::vector<std::filesystem::path> collectDirectoryInputs(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> inputs;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && isSupportedImagePath(entry.path())) {
            inputs.push_back(entry.path());
        }
    }
    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

int registerBatch(const std::vector<std::string>& args) {
    struct FrameSummary {
        bool ok = false;
        bool reference = false;
        bool usedFallback = false;
        std::size_t matches = 0;
        std::size_t detectedReferenceStars = 0;
        std::size_t detectedMovingStars = 0;
        float inlierRatio = 0.0F;
        std::string input;
        std::string output;
        std::string errorCode;
        std::string message;
    };

    std::optional<std::filesystem::path> referencePath;
    std::optional<std::filesystem::path> inputDirectory;
    std::optional<std::filesystem::path> outputDirectory;
    std::vector<std::filesystem::path> inputFiles;
    photonstack::RegistrationOptions options;
    std::string mode = "distortion";
    std::string outputFormat = "tiff";
    std::string prefix = "aligned-";
    bool includeReference = true;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--reference") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--reference requires an image path");
                return 1;
            }
            referencePath = args[++i];
        } else if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires a directory path");
                return 1;
            }
            inputDirectory = args[++i];
        } else if (arg == "--output-dir") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output-dir requires a directory path");
                return 1;
            }
            outputDirectory = args[++i];
        } else if (arg == "--mode") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--mode requires translation, similarity, affine, or distortion");
                return 1;
            }
            mode = args[++i];
            if (mode != "translation" && mode != "similarity" && mode != "affine" && mode != "distortion") {
                printError("ArgumentInvalid", "--mode must be translation, similarity, affine, or distortion");
                return 1;
            }
        } else if (arg == "--output-format") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output-format requires tiff, png, or fits");
                return 1;
            }
            outputFormat = args[++i];
            std::transform(outputFormat.begin(), outputFormat.end(), outputFormat.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (outputFormat != "tiff" && outputFormat != "tif" && outputFormat != "png" &&
                outputFormat != "fits" && outputFormat != "fit" && outputFormat != "fts") {
                printError("ArgumentInvalid", "--output-format must be tiff, png, or fits");
                return 1;
            }
        } else if (arg == "--prefix") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--prefix requires a filename prefix");
                return 1;
            }
            prefix = args[++i];
        } else if (arg == "--include-reference") {
            if (!parseOnOff(args, i, "--include-reference", includeReference)) {
                return 1;
            }
        } else if (arg == "--match-tolerance") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--match-tolerance requires a positive number");
                return 1;
            }
            try {
                options.matchTolerance = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--match-tolerance must be a positive number");
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                options.starDetection.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--min-peak requires a positive number");
                return 1;
            }
            try {
                options.starDetection.minPeak = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--min-peak must be a positive number");
                return 1;
            }
        } else if (arg == "--max-stars") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--max-stars requires a positive integer");
                return 1;
            }
            try {
                options.starDetection.maxStars = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--max-stars must be a positive integer");
                return 1;
            }
        } else if (arg == "--minimum-matches") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--minimum-matches requires a positive integer");
                return 1;
            }
            try {
                options.minimumMatches = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--minimum-matches must be a positive integer");
                return 1;
            }
            if (options.minimumMatches == 0) {
                printError("ArgumentInvalid", "--minimum-matches must be greater than zero");
                return 1;
            }
        } else {
            inputFiles.emplace_back(arg);
        }
    }

    if (!referencePath.has_value()) {
        printError("ArgumentMissing", "register-batch requires --reference <image>");
        return 1;
    }
    if (!outputDirectory.has_value()) {
        printError("ArgumentMissing", "register-batch requires --output-dir <directory>");
        return 1;
    }
    if (inputDirectory.has_value()) {
        if (!std::filesystem::is_directory(*inputDirectory)) {
            printError("InputPathInvalid", "--input must point to a directory");
            return 1;
        }
        auto directoryInputs = collectDirectoryInputs(*inputDirectory);
        inputFiles.insert(inputFiles.end(), directoryInputs.begin(), directoryInputs.end());
    }
    if (inputFiles.empty()) {
        printError("InputMissing", "No input images were provided");
        return 1;
    }

    try {
        std::filesystem::create_directories(*outputDirectory);
    } catch (const std::exception& error) {
        printError("OutputPathInvalid", std::string("Could not create output directory: ") + error.what());
        return 1;
    }

    const bool fitsOutput = outputFormat == "fits" || outputFormat == "fit" || outputFormat == "fts";
    const auto outputExtension = outputFormat == "png" ? std::string(".png")
                                                        : (fitsOutput ? std::string(".fits") : std::string(".tiff"));
    if (fitsOutput) {
        outputFormat = "fits";
    } else if (outputFormat == "tif") {
        outputFormat = "tiff";
    }
    std::vector<std::string> usedOutputPaths;
    auto makeOutputPath = [&](const std::filesystem::path& source) {
        const std::string stem = source.stem().empty() ? std::string("frame") : source.stem().string();
        auto candidate = *outputDirectory / (prefix + stem + outputExtension);
        int suffix = 2;
        while (std::find(usedOutputPaths.begin(), usedOutputPaths.end(), candidate.string()) != usedOutputPaths.end()) {
            candidate = *outputDirectory / (prefix + stem + "-" + std::to_string(suffix++) + outputExtension);
        }
        usedOutputPaths.push_back(candidate.string());
        return candidate;
    };

    auto sameFile = [](const std::filesystem::path& left, const std::filesystem::path& right) {
        try {
            return std::filesystem::equivalent(left, right);
        } catch (const std::exception&) {
            return left.lexically_normal() == right.lexically_normal();
        }
    };

    const photonstack::ImageCodec codec;
    const auto readOptions = scientificFitsReadOptions();
    const auto referenceRead = codec.read(*referencePath, readOptions);
    if (!referenceRead.ok) {
        printError(referenceRead.errorCode, referenceRead.message);
        return 1;
    }

    const photonstack::Registration registration;
    std::vector<FrameSummary> frames;
    std::size_t alignedFrames = 0;
    std::size_t failedFrames = 0;
    const std::size_t totalSteps = inputFiles.size() + (includeReference ? 1 : 0);
    std::size_t currentStep = 0;

    if (includeReference) {
        const auto referenceOutput = makeOutputPath(*referencePath);
        const auto writeResult = codec.write(referenceRead.image, referenceOutput);
        FrameSummary summary;
        summary.reference = true;
        summary.input = referencePath->string();
        summary.output = referenceOutput.string();
        summary.ok = writeResult.ok;
        summary.errorCode = writeResult.errorCode;
        summary.message = writeResult.ok ? "Reference frame copied into aligned batch" : writeResult.message;
        frames.push_back(summary);
        alignedFrames += writeResult.ok ? 1 : 0;
        failedFrames += writeResult.ok ? 0 : 1;
        ++currentStep;
        printProgressEvent("register-batch", "register-batch", "reference", static_cast<double>(currentStep) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)), currentStep, totalSteps);
    }

    for (const auto& inputPath : inputFiles) {
        if (sameFile(inputPath, *referencePath)) {
            ++currentStep;
            printProgressEvent("register-batch", "register-batch", "skip-reference",
                               static_cast<double>(currentStep) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                               currentStep, totalSteps);
            continue;
        }

        FrameSummary summary;
        summary.input = inputPath.string();
        const auto outputPath = makeOutputPath(inputPath);
        summary.output = outputPath.string();

        const auto movingRead = codec.read(inputPath, readOptions);
        if (!movingRead.ok) {
            summary.errorCode = movingRead.errorCode;
            summary.message = movingRead.message;
            frames.push_back(summary);
            ++failedFrames;
            ++currentStep;
            printProgressEvent("register-batch", "register-batch", "read-failed",
                               static_cast<double>(currentStep) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                               currentStep, totalSteps);
            continue;
        }

        photonstack::DistortionTransform distortionTransform;
        const auto registrationResult =
            mode == "distortion"
                ? registration.estimateDistortion(referenceRead.image, movingRead.image, distortionTransform, options)
                : (mode == "affine"
                       ? registration.estimateAffine(referenceRead.image, movingRead.image, options)
                       : (mode == "similarity" ? registration.estimateSimilarity(referenceRead.image, movingRead.image, options)
                                               : registration.estimateTranslation(referenceRead.image, movingRead.image, options)));
        if (!registrationResult.ok) {
            summary.errorCode = registrationResult.errorCode;
            summary.message = registrationResult.message;
            summary.detectedReferenceStars = registrationResult.detectedReferenceStars;
            summary.detectedMovingStars = registrationResult.detectedMovingStars;
            frames.push_back(summary);
            ++failedFrames;
            ++currentStep;
            printProgressEvent("register-batch", "register-batch", "align-failed",
                               static_cast<double>(currentStep) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                               currentStep, totalSteps);
            continue;
        }

        const auto writeResult = writeRegisteredFrame(codec, registration, movingRead.image, outputPath, mode,
                                                      registrationResult, distortionTransform);
        summary.ok = writeResult.ok;
        summary.errorCode = writeResult.errorCode;
        summary.message = writeResult.ok ? registrationResult.message : writeResult.message;
        summary.matches = registrationResult.matches;
        summary.detectedReferenceStars = registrationResult.detectedReferenceStars;
        summary.detectedMovingStars = registrationResult.detectedMovingStars;
        summary.inlierRatio = registrationResult.inlierRatio;
        summary.usedFallback = registrationResult.usedFallback;
        frames.push_back(summary);
        alignedFrames += writeResult.ok ? 1 : 0;
        failedFrames += writeResult.ok ? 0 : 1;
        ++currentStep;
        printProgressEvent("register-batch", "register-batch", writeResult.ok ? "aligned" : "write-failed",
                           static_cast<double>(currentStep) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                           currentStep, totalSteps);
    }

    std::cout << "{\n"
              << "  \"type\": \"" << (failedFrames == 0 ? "complete" : "partial") << "\",\n"
              << "  \"command\": \"register-batch\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"mode\": \"" << jsonEscape(mode) << "\",\n"
              << "  \"frames\": " << frames.size() << ",\n"
              << "  \"alignedFrames\": " << alignedFrames << ",\n"
              << "  \"failedFrames\": " << failedFrames << ",\n"
              << "  \"outputFormat\": \"" << jsonEscape(outputFormat) << "\",\n"
              << "  \"outputDirectory\": \"" << jsonEscape(outputDirectory->string()) << "\",\n"
              << "  \"items\": [\n";
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        std::cout << "    {\"ok\":" << (frame.ok ? "true" : "false")
                  << ",\"reference\":" << (frame.reference ? "true" : "false")
                  << ",\"input\":\"" << jsonEscape(frame.input) << "\""
                  << ",\"output\":\"" << jsonEscape(frame.output) << "\""
                  << ",\"matches\":" << frame.matches
                  << ",\"detectedReferenceStars\":" << frame.detectedReferenceStars
                  << ",\"detectedMovingStars\":" << frame.detectedMovingStars
                  << ",\"inlierRatio\":" << frame.inlierRatio
                  << ",\"usedFallback\":" << (frame.usedFallback ? "true" : "false")
                  << ",\"errorCode\":\"" << jsonEscape(frame.errorCode) << "\""
                  << ",\"message\":\"" << jsonEscape(frame.message) << "\"}";
        if (index + 1 < frames.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
    return failedFrames == 0 ? 0 : 1;
}

int stackImages(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputDirectory;
    std::optional<std::filesystem::path> outputPath;
    std::string method = "average";
    photonstack::StackOptions stackOptions;
    std::vector<std::filesystem::path> inputFiles;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires a directory path");
                return 1;
            }
            inputDirectory = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--method") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--method requires a value");
                return 1;
            }
            method = args[++i];
        } else if (arg == "--sigma-low") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-low requires a positive number");
                return 1;
            }
            try {
                stackOptions.sigma.sigmaLow = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-low must be a positive number");
                return 1;
            }
        } else if (arg == "--sigma-high") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-high requires a positive number");
                return 1;
            }
            try {
                stackOptions.sigma.sigmaHigh = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-high must be a positive number");
                return 1;
            }
        } else if (arg == "--percentile-low") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--percentile-low requires a number between 0 and 1");
                return 1;
            }
            try {
                stackOptions.percentile.low = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--percentile-low must be a number between 0 and 1");
                return 1;
            }
        } else if (arg == "--percentile-high") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--percentile-high requires a number between 0 and 1");
                return 1;
            }
            try {
                stackOptions.percentile.high = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--percentile-high must be a number between 0 and 1");
                return 1;
            }
        } else if (arg == "--align") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--align requires a value");
                return 1;
            }
            const auto alignMode = args[++i];
            if (alignMode == "translation") {
                stackOptions.alignTranslation = true;
                stackOptions.alignSimilarity = false;
                stackOptions.alignAffine = false;
                stackOptions.alignDistortion = false;
            } else if (alignMode == "similarity") {
                stackOptions.alignSimilarity = true;
                stackOptions.alignTranslation = false;
                stackOptions.alignAffine = false;
                stackOptions.alignDistortion = false;
            } else if (alignMode == "affine") {
                stackOptions.alignAffine = true;
                stackOptions.alignSimilarity = false;
                stackOptions.alignTranslation = false;
                stackOptions.alignDistortion = false;
            } else if (alignMode == "distortion") {
                stackOptions.alignDistortion = true;
                stackOptions.alignAffine = false;
                stackOptions.alignSimilarity = false;
                stackOptions.alignTranslation = false;
            } else {
                printError("ArgumentInvalid", "--align must be translation, similarity, affine, or distortion");
                return 1;
            }
        } else if (arg == "--match-tolerance") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--match-tolerance requires a positive number");
                return 1;
            }
            try {
                stackOptions.registration.matchTolerance = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--match-tolerance must be a positive number");
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                stackOptions.registration.starDetection.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--minimum-matches") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--minimum-matches requires a positive integer");
                return 1;
            }
            try {
                stackOptions.registration.minimumMatches = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--minimum-matches must be a positive integer");
                return 1;
            }
            if (stackOptions.registration.minimumMatches == 0) {
                printError("ArgumentInvalid", "--minimum-matches must be greater than zero");
                return 1;
            }
        } else {
            inputFiles.emplace_back(arg);
        }
    }

    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "stack requires --output <image>");
        return 1;
    }
    if (method != "average" && method != "weighted" && method != "median" && method != "sigma" &&
        method != "winsorized" && method != "percentile") {
        printError("MethodUnsupported",
                   "Supported stack methods are average, weighted, median, sigma, winsorized, and percentile");
        return 1;
    }

    if (inputDirectory.has_value()) {
        if (!std::filesystem::is_directory(*inputDirectory)) {
            printError("InputPathInvalid", "--input must point to a directory");
            return 1;
        }
        auto directoryInputs = collectDirectoryInputs(*inputDirectory);
        inputFiles.insert(inputFiles.end(), directoryInputs.begin(), directoryInputs.end());
    }

    if (inputFiles.empty()) {
        printError("InputMissing", "No input images were provided");
        return 1;
    }

    const photonstack::Stacker stacker;
    if (method == "average") {
        stackOptions.method = photonstack::StackMethod::Average;
    } else if (method == "weighted") {
        stackOptions.method = photonstack::StackMethod::WeightedAverage;
    } else if (method == "median") {
        stackOptions.method = photonstack::StackMethod::Median;
    } else if (method == "sigma") {
        stackOptions.method = photonstack::StackMethod::SigmaClip;
    } else if (method == "winsorized") {
        stackOptions.method = photonstack::StackMethod::WinsorizedSigmaClip;
        stackOptions.winsorizedSigma.sigmaLow = stackOptions.sigma.sigmaLow;
        stackOptions.winsorizedSigma.sigmaHigh = stackOptions.sigma.sigmaHigh;
    } else {
        stackOptions.method = photonstack::StackMethod::PercentileClip;
    }
    stackOptions.progress = [&](const photonstack::StackProgress& event) {
        const char* stage = "read";
        switch (event.stage) {
        case photonstack::StackProgressStage::Reading:
            stage = "read";
            break;
        case photonstack::StackProgressStage::Aligning:
            stage = "align";
            break;
        case photonstack::StackProgressStage::Caching:
            stage = "cache";
            break;
        case photonstack::StackProgressStage::Accumulating:
            stage = "accumulate";
            break;
        case photonstack::StackProgressStage::Combining:
            stage = "combine";
            break;
        case photonstack::StackProgressStage::Finalizing:
            stage = "finalize";
            break;
        }
        const auto step = event.frame > 0 ? std::optional<std::size_t>(event.frame) : std::nullopt;
        const auto total = event.frame > 0 ? std::optional<std::size_t>(event.frameCount) : std::nullopt;
        printProgressEvent("stack", "stack", stage, 0.02 + event.progress * 0.88, step, total);
    };
    const auto stackResult = stacker.stack(inputFiles, stackOptions);
    if (!stackResult.ok) {
        printError(stackResult.errorCode, stackResult.message);
        return 1;
    }

    const photonstack::ImageCodec codec;
    photonstack::ImageWriteResult writeResult;
    printProgressEvent("stack", "stack", "write", 0.91);
    if (stackResult.image.channels == 4) {
        writeResult = codec.writeRows(stackResult.image, *outputPath,
                                      [&](const photonstack::ImageRowConsumer& consumer) {
            const auto rowSamples = static_cast<std::size_t>(stackResult.image.width) * stackResult.image.channels;
            const auto interval = std::max<std::uint32_t>(1, stackResult.image.height / 100);
            for (std::uint32_t row = 0; row < stackResult.image.height; ++row) {
                const auto offset = static_cast<std::size_t>(row) * rowSamples;
                if (!consumer(row, stackResult.image.pixels.data() + offset, rowSamples)) {
                    return false;
                }
                const auto completed = row + 1;
                if (completed == stackResult.image.height || completed % interval == 0) {
                    const double fraction = static_cast<double>(completed) / stackResult.image.height;
                    printProgressEvent("stack", "stack", "write", 0.91 + fraction * 0.08,
                                       completed, stackResult.image.height);
                }
            }
            return true;
        });
    }
    if (stackResult.image.channels != 4 ||
        (!writeResult.ok && writeResult.errorCode == "ImageRowWriteUnsupported")) {
        writeResult = codec.write(stackResult.image, *outputPath);
        if (writeResult.ok) {
            printProgressEvent("stack", "stack", "write", 0.99,
                               stackResult.image.height, stackResult.image.height);
        }
    }
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"stack\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"method\": \"" << jsonEscape(method) << "\",\n"
              << "  \"align\": \""
              << (stackOptions.alignDistortion ? "distortion"
                                               : (stackOptions.alignAffine
                                                      ? "affine"
                                                      : (stackOptions.alignSimilarity
                                                             ? "similarity"
                                                             : (stackOptions.alignTranslation ? "translation" : "none"))))
              << "\",\n"
              << "  \"frames\": " << inputFiles.size() << ",\n"
              << "  \"alignedFrames\": " << stackResult.alignedFrames << ",\n"
              << "  \"alignmentFallbacks\": " << stackResult.alignmentFallbacks << ",\n"
              << "  \"minimumAlignmentMatches\": " << stackResult.minimumAlignmentMatches << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int drizzleImages(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> outputPath;
    photonstack::DrizzleOptions options;
    std::vector<std::filesystem::path> inputFiles;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--scale") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--scale requires an integer between 1 and 4");
                return 1;
            }
            try {
                options.scale = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--scale must be an integer between 1 and 4");
                return 1;
            }
        } else if (arg == "--align") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--align requires none, translation, similarity, affine, or distortion");
                return 1;
            }
            const auto mode = args[++i];
            if (mode == "none") {
                options.alignment = photonstack::DrizzleAlignment::None;
            } else if (mode == "translation") {
                options.alignment = photonstack::DrizzleAlignment::Translation;
            } else if (mode == "similarity") {
                options.alignment = photonstack::DrizzleAlignment::Similarity;
            } else if (mode == "affine") {
                options.alignment = photonstack::DrizzleAlignment::Affine;
            } else if (mode == "distortion") {
                options.alignment = photonstack::DrizzleAlignment::Distortion;
            } else {
                printError("ArgumentInvalid", "--align must be none, translation, similarity, affine, or distortion");
                return 1;
            }
        } else if (arg == "--no-align") {
            options.alignment = photonstack::DrizzleAlignment::None;
        } else if (arg == "--pixfrac") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--pixfrac requires a number greater than 0 and no greater than 1");
                return 1;
            }
            try {
                options.pixfrac = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--pixfrac must be a number greater than 0 and no greater than 1");
                return 1;
            }
        } else if (arg == "--match-tolerance") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--match-tolerance requires a positive number");
                return 1;
            }
            try {
                options.registration.matchTolerance = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--match-tolerance must be a positive number");
                return 1;
            }
        } else {
            inputFiles.emplace_back(arg);
        }
    }

    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "drizzle requires --output <image>");
        return 1;
    }
    if (inputFiles.empty()) {
        printError("InputMissing", "drizzle requires at least one input image");
        return 1;
    }

    options.progress = [&](const photonstack::DrizzleProgress& event) {
        const char* stage = "read";
        switch (event.stage) {
        case photonstack::DrizzleProgressStage::Reading:
            stage = "read";
            break;
        case photonstack::DrizzleProgressStage::Aligning:
            stage = "align";
            break;
        case photonstack::DrizzleProgressStage::Accumulating:
            stage = "accumulate";
            break;
        case photonstack::DrizzleProgressStage::Normalizing:
            stage = "normalize";
            break;
        }
        const auto step = event.frame > 0 ? std::optional<std::size_t>(event.frame) : std::nullopt;
        const auto total = event.frame > 0 ? std::optional<std::size_t>(event.frameCount) : std::nullopt;
        printProgressEvent("drizzle", "drizzle", stage, 0.02 + event.progress * 0.88, step, total);
    };

    const photonstack::DrizzleStacker drizzle;
    const auto result = drizzle.drizzle(inputFiles, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    const photonstack::ImageCodec codec;
    photonstack::ImageWriteResult writeResult;
    printProgressEvent("drizzle", "drizzle", "write", 0.91);
    if (result.image.channels == 4) {
        writeResult = codec.writeRows(result.image, *outputPath, [&](const photonstack::ImageRowConsumer& consumer) {
            const auto rowSamples = static_cast<std::size_t>(result.image.width) * result.image.channels;
            const auto interval = std::max<std::uint32_t>(1, result.image.height / 100);
            for (std::uint32_t row = 0; row < result.image.height; ++row) {
                const auto offset = static_cast<std::size_t>(row) * rowSamples;
                if (!consumer(row, result.image.pixels.data() + offset, rowSamples)) {
                    return false;
                }
                const auto completed = row + 1;
                if (completed == result.image.height || completed % interval == 0) {
                    const double fraction = static_cast<double>(completed) / result.image.height;
                    printProgressEvent("drizzle", "drizzle", "write", 0.91 + fraction * 0.08,
                                       completed, result.image.height);
                }
            }
            return true;
        });
    }
    if (result.image.channels != 4 ||
        (!writeResult.ok && writeResult.errorCode == "ImageRowWriteUnsupported")) {
        writeResult = codec.write(result.image, *outputPath);
        if (writeResult.ok) {
            printProgressEvent("drizzle", "drizzle", "write", 0.99, result.image.height, result.image.height);
        }
    }
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"drizzle\",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"scale\": " << options.scale << ",\n"
              << "  \"pixfrac\": " << options.pixfrac << ",\n"
              << "  \"alignment\": \""
              << (options.alignment == photonstack::DrizzleAlignment::Distortion
                      ? "distortion"
                      : (options.alignment == photonstack::DrizzleAlignment::Affine
                             ? "affine"
                             : (options.alignment == photonstack::DrizzleAlignment::Similarity
                                    ? "similarity"
                                    : (options.alignment == photonstack::DrizzleAlignment::Translation
                                           ? "translation"
                                           : "none"))))
              << "\",\n"
              << "  \"alignedFrames\": " << result.alignedFrames << ",\n"
              << "  \"alignmentFallbacks\": " << result.alignmentFallbacks << ",\n"
              << "  \"minimumAlignmentMatches\": " << result.minimumAlignmentMatches << ",\n"
              << "  \"width\": " << result.image.width << ",\n"
              << "  \"height\": " << result.image.height << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

bool parseOnOff(const std::vector<std::string>& args, std::size_t& index, const std::string& name, bool& target) {
    if (index + 1 >= args.size()) {
        printError("ArgumentMissing", name + " requires on or off");
        return false;
    }
    const auto value = args[++index];
    if (value == "on" || value == "true" || value == "1") {
        target = true;
        return true;
    }
    if (value == "off" || value == "false" || value == "0") {
        target = false;
        return true;
    }
    printError("ArgumentInvalid", name + " must be on or off");
    return false;
}

std::vector<std::string> splitCommaList(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty()) {
            parts.push_back(token);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

std::vector<std::string> splitDelimitedList(const std::string& value, char delimiter, bool preserveEmpty = false) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto position = value.find(delimiter, start);
        const auto token = value.substr(
            start,
            position == std::string::npos ? std::string::npos : position - start
        );
        if (preserveEmpty || !token.empty()) {
            parts.push_back(token);
        }
        if (position == std::string::npos) {
            break;
        }
        start = position + 1;
    }
    return parts;
}

photonstack::ArtifactTrailKind parseArtifactTrailKind(const std::string& value) {
    if (value == "airplane") {
        return photonstack::ArtifactTrailKind::Airplane;
    }
    if (value == "drone") {
        return photonstack::ArtifactTrailKind::Drone;
    }
    if (value == "satellite") {
        return photonstack::ArtifactTrailKind::Satellite;
    }
    if (value == "meteor") {
        return photonstack::ArtifactTrailKind::Meteor;
    }
    throw std::invalid_argument("unknown artifact trail kind");
}

std::vector<photonstack::ArtifactTrail> parseDetectedArtifactTrailsV1(const std::string& value) {
    std::vector<photonstack::ArtifactTrail> trails;
    if (value.empty()) {
        return trails;
    }
    for (const auto& encodedTrail : splitDelimitedList(value, ';')) {
        const auto fields = splitDelimitedList(encodedTrail, '|', true);
        if (fields.size() != 13) {
            throw std::invalid_argument("artifact trail field count");
        }
        photonstack::ArtifactTrail trail;
        trail.kind = parseArtifactTrailKind(fields[0]);
        trail.confidence = parseFiniteFloat(fields[1]);
        trail.x1 = parseFiniteFloat(fields[2]);
        trail.y1 = parseFiniteFloat(fields[3]);
        trail.x2 = parseFiniteFloat(fields[4]);
        trail.y2 = parseFiniteFloat(fields[5]);
        trail.length = parseFiniteFloat(fields[6]);
        trail.width = parseFiniteFloat(fields[7]);
        trail.meanBrightness = parseFiniteFloat(fields[8]);
        trail.weight = parseFiniteFloat(fields[9]);
        trail.peakPosition = parseFiniteFloat(fields[10]);
        trail.taperScore = parseFiniteFloat(fields[11]);
        trail.angleRadians = std::atan2(trail.y2 - trail.y1, trail.x2 - trail.x1);
        if (trail.confidence < 0.0F || trail.confidence > 1.0F || trail.length < 0.0F || trail.width <= 0.0F ||
            trail.peakPosition < 0.0F || trail.peakPosition > 1.0F) {
            throw std::invalid_argument("artifact trail field range");
        }
        if (!fields[12].empty()) {
            for (const auto& encodedPoint : splitCommaList(fields[12])) {
                const auto coordinates = splitDelimitedList(encodedPoint, ':', true);
                if (coordinates.size() != 2) {
                    throw std::invalid_argument("artifact trail path point");
                }
                trail.path.push_back({
                    .x = parseFiniteFloat(coordinates[0]),
                    .y = parseFiniteFloat(coordinates[1]),
                });
            }
        }
        trails.push_back(std::move(trail));
    }
    return trails;
}

int processArtifacts(const std::string& mode, const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::ArtifactTrailOptions options;
    photonstack::ImageReadOptions readOptions;
    readOptions.raw.linearOutput = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };

        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--sigma-threshold") {
            if (!parseFloat(options.sigmaThreshold, "--sigma-threshold")) {
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (!parseFloat(options.minPeak, "--min-peak")) {
                return 1;
            }
        } else if (arg == "--min-length") {
            if (!parseFloat(options.minLength, "--min-length")) {
                return 1;
            }
        } else if (arg == "--airplane-length") {
            if (!parseFloat(options.airplaneLength, "--airplane-length")) {
                return 1;
            }
        } else if (arg == "--max-width") {
            if (!parseFloat(options.maxWidth, "--max-width")) {
                return 1;
            }
        } else if (arg == "--mask-radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--mask-radius requires a positive integer");
                return 1;
            }
            try {
                options.maskRadius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--mask-radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--inpaint-radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--inpaint-radius requires a positive integer");
                return 1;
            }
            try {
                options.inpaintRadius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--inpaint-radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--preserve-meteors") {
            if (!parseOnOff(args, i, "--preserve-meteors", options.preserveMeteors)) {
                return 1;
            }
        } else if (arg == "--include-meteors") {
            if (!parseOnOff(args, i, "--include-meteors", options.includeMeteors)) {
                return 1;
            }
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, readOptions.raw)) {
                return 1;
            }
        } else if (arg == "--remove-kinds") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--remove-kinds requires a comma-separated list");
                return 1;
            }
            options.removeAirplanes = false;
            options.removeDrones = false;
            options.removeSatellites = false;
            options.removeMeteors = false;
            for (const auto& kind : splitCommaList(args[++i])) {
                if (kind == "airplane" || kind == "airplanes") {
                    options.removeAirplanes = true;
                } else if (kind == "drone" || kind == "drones") {
                    options.removeDrones = true;
                } else if (kind == "satellite" || kind == "satellites") {
                    options.removeSatellites = true;
                } else if (kind == "meteor" || kind == "meteors") {
                    options.removeMeteors = true;
                    options.preserveMeteors = false;
                } else {
                    printError("ArgumentInvalid", "--remove-kinds supports airplane, drone, satellite, and meteor");
                    return 1;
                }
            }
        } else if (arg == "--selected-indices") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--selected-indices requires a comma-separated list");
                return 1;
            }
            try {
                for (const auto& token : splitCommaList(args[++i])) {
                    options.selectedIndices.push_back(parseUnsignedInteger<std::size_t>(token));
                }
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--selected-indices must contain zero-based integer indices");
                return 1;
            }
        } else if (arg == "--detected-trails-v1") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--detected-trails-v1 requires encoded trail geometry");
                return 1;
            }
            try {
                options.detectedTrails = parseDetectedArtifactTrailsV1(args[++i]);
                options.useDetectedTrails = true;
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--detected-trails-v1 contains invalid trail geometry");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown artifacts argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "artifacts " + mode + " requires --input <image>");
        return 1;
    }
    if (mode == "remove" && !outputPath.has_value()) {
        printError("ArgumentMissing", "artifacts remove requires --output <image>");
        return 1;
    }
    const bool scientificBridge = mode == "remove" && photonstack::isFitsPath(*outputPath);
    if (scientificBridge && !photonstack::isFitsPath(*inputPath)) {
        printError(
            "ScientificBridgeUnsupported",
            "Scientific artifact removal requires a FITS input when the output is FITS"
        );
        return 1;
    }
    if (scientificBridge) {
        readOptions.fits.maskNonFinitePixels = true;
    }

    const std::string progressCommand = "artifacts " + mode;
    printProgressEvent("artifacts", progressCommand, "read", 0.02);
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, readOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }
    printProgressEvent("artifacts", progressCommand, "read", 0.07);
    std::optional<photonstack::ImageBuffer> scientificSource;
    if (scientificBridge) {
        const auto scientificRead = codec.read(*inputPath, scientificFitsReadOptions());
        if (!scientificRead.ok) {
            printError(scientificRead.errorCode, scientificRead.message);
            return 1;
        }
        scientificSource = scientificRead.image;
    }

    const double engineEnd = mode == "remove" ? 0.90 : 0.98;
    options.progress = [&](const photonstack::ArtifactTrailProgress& event) {
        const char* stage = "analyze";
        switch (event.stage) {
        case photonstack::ArtifactTrailProgressStage::Analyzing:
            stage = "analyze";
            break;
        case photonstack::ArtifactTrailProgressStage::Components:
            stage = "components";
            break;
        case photonstack::ArtifactTrailProgressStage::Navigation:
            stage = "navigation";
            break;
        case photonstack::ArtifactTrailProgressStage::PatternSearch:
            stage = "patterns";
            break;
        case photonstack::ArtifactTrailProgressStage::Refining:
            stage = "refine";
            break;
        case photonstack::ArtifactTrailProgressStage::Masking:
            stage = "mask";
            break;
        case photonstack::ArtifactTrailProgressStage::Inpainting:
            stage = "inpaint";
            break;
        case photonstack::ArtifactTrailProgressStage::Cleaning:
            stage = "clean";
            break;
        }
        std::optional<std::size_t> step;
        std::optional<std::size_t> total;
        if (event.item > 0 && event.itemCount > 0) {
            step = event.item;
            total = event.itemCount;
        } else if (event.row > 0 && event.rowCount > 0) {
            step = event.row;
            total = event.rowCount;
        }
        printProgressEvent("artifacts", progressCommand, stage,
                           0.08 + event.progress * (engineEnd - 0.08), step, total);
    };

    const photonstack::ArtifactTrailRemover remover;
    auto result = mode == "remove" ? remover.remove(readResult.image, options) : remover.detect(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }
    std::size_t bridgedSamples = 0;
    if (mode == "remove") {
        if (scientificBridge) {
            const auto bridgeResult = photonstack::ScientificDisplayBridge().applyCorrections(
                *scientificSource,
                readResult.image,
                result.image
            );
            if (!bridgeResult.ok) {
                printError(bridgeResult.errorCode, bridgeResult.message);
                return 1;
            }
            result.image = bridgeResult.image;
            bridgedSamples = bridgeResult.correctedSamples;
        }
        photonstack::ImageWriteResult writeResult;
        printProgressEvent("artifacts", "artifacts remove", "write", 0.91);
        if (result.image.channels == 4) {
            writeResult = codec.writeRows(result.image, *outputPath,
                                          [&](const photonstack::ImageRowConsumer& consumer) {
                const auto rowSamples = static_cast<std::size_t>(result.image.width) * result.image.channels;
                const auto interval = std::max<std::uint32_t>(1, result.image.height / 100);
                for (std::uint32_t row = 0; row < result.image.height; ++row) {
                    const auto offset = static_cast<std::size_t>(row) * rowSamples;
                    if (!consumer(row, result.image.pixels.data() + offset, rowSamples)) {
                        return false;
                    }
                    const auto completed = row + 1;
                    if (completed == result.image.height || completed % interval == 0) {
                        const double fraction = static_cast<double>(completed) / result.image.height;
                        printProgressEvent("artifacts", "artifacts remove", "write",
                                           0.91 + fraction * 0.08, completed, result.image.height);
                    }
                }
                return true;
            });
        }
        if (result.image.channels != 4 ||
            (!writeResult.ok && writeResult.errorCode == "ImageRowWriteUnsupported")) {
            writeResult = codec.write(result.image, *outputPath);
            if (writeResult.ok) {
                printProgressEvent("artifacts", "artifacts remove", "write", 0.99,
                                   result.image.height, result.image.height);
            }
        }
        if (!writeResult.ok) {
            printError(writeResult.errorCode, writeResult.message);
            return 1;
        }
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"artifacts " << jsonEscape(mode) << "\",\n"
              << "  \"valueDomain\": \"" << (scientificBridge ? "scientific-bridge" : "display") << "\",\n"
              << "  \"bridgedSamples\": " << bridgedSamples << ",\n"
              << "  \"width\": " << readResult.image.width << ",\n"
              << "  \"height\": " << readResult.image.height << ",\n"
              << "  \"trails\": " << result.trails.size() << ",\n"
              << "  \"removedTrails\": " << result.removedTrails << ",\n"
              << "  \"protectedMeteors\": " << result.protectedMeteors << ",\n"
              << "  \"items\": [\n";
    for (std::size_t i = 0; i < result.trails.size(); ++i) {
        const auto& trail = result.trails[i];
        std::cout << "    {\"kind\":\"" << photonstack::toString(trail.kind) << "\","
                  << "\"confidence\":" << trail.confidence << ","
                  << "\"x1\":" << trail.x1 << ","
                  << "\"y1\":" << trail.y1 << ","
                  << "\"x2\":" << trail.x2 << ","
                  << "\"y2\":" << trail.y2 << ","
                  << "\"length\":" << trail.length << ","
                  << "\"width\":" << trail.width << ","
                  << "\"meanBrightness\":" << trail.meanBrightness << ","
                  << "\"weight\":" << trail.weight << ","
                  << "\"peakPosition\":" << trail.peakPosition << ","
                  << "\"taperScore\":" << trail.taperScore << ","
                  << "\"colorVariance\":" << trail.colorVariance << ","
                  << "\"warmEvidence\":" << trail.warmEvidence << ","
                  << "\"coherentParallelGroup\":" << (trail.coherentParallelGroup ? "true" : "false") << ","
                  << "\"verifiedContinuousSatellite\":"
                  << (trail.verifiedContinuousSatellite ? "true" : "false") << ","
                  << "\"verifiedSegmentedSatelliteChain\":"
                  << (trail.verifiedSegmentedSatelliteChain ? "true" : "false") << ","
                  << "\"strongAsymmetricMeteor\":" << (trail.strongAsymmetricMeteor ? "true" : "false");
        if (trail.path.size() >= 2) {
            std::cout << ",\"path\":[";
            for (std::size_t pathIndex = 0; pathIndex < trail.path.size(); ++pathIndex) {
                const auto& point = trail.path[pathIndex];
                std::cout << "{\"x\":" << point.x << ",\"y\":" << point.y << "}";
                if (pathIndex + 1 < trail.path.size()) {
                    std::cout << ",";
                }
            }
            std::cout << "]";
        }
        std::cout << "}";
        if (i + 1 < result.trails.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]";
    if (outputPath.has_value()) {
        std::cout << ",\n  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n";
    } else {
        std::cout << "\n";
    }
    std::cout << "}\n";
    return 0;
}

int cleanArtifactSequence(const std::vector<std::string>& args) {
    struct FrameSummary {
        bool ok = false;
        std::string input;
        std::string output;
        std::string errorCode;
        std::string message;
        std::vector<std::size_t> selectedIndices;
        std::size_t trails = 0;
        std::size_t removedTrails = 0;
        std::size_t protectedMeteors = 0;
        std::size_t recurrentTrails = 0;
    };

    std::optional<std::filesystem::path> inputDirectory;
    std::optional<std::filesystem::path> outputDirectory;
    std::vector<std::filesystem::path> inputFiles;
    photonstack::ArtifactTrailOptions options;
    photonstack::ImageReadOptions readOptions;
    readOptions.raw.linearOutput = false;
    std::string outputFormat = "tiff";
    std::string prefix = "clean-";
    float minWeight = 0.30F;
    float sequenceMinLength = 36.0F;
    float sequenceMinBrightness = 0.020F;
    std::size_t recurrenceThreshold = 2;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };

        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires a directory path");
                return 1;
            }
            inputDirectory = args[++i];
        } else if (arg == "--output-dir") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output-dir requires a directory path");
                return 1;
            }
            outputDirectory = args[++i];
        } else if (arg == "--output-format") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output-format requires tiff or png");
                return 1;
            }
            outputFormat = args[++i];
            std::transform(outputFormat.begin(), outputFormat.end(), outputFormat.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (outputFormat != "tiff" && outputFormat != "tif" && outputFormat != "png") {
                printError("ArgumentInvalid", "--output-format must be tiff or png");
                return 1;
            }
        } else if (arg == "--prefix") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--prefix requires a filename prefix");
                return 1;
            }
            prefix = args[++i];
        } else if (arg == "--min-weight") {
            if (!parseFloat(minWeight, "--min-weight")) {
                return 1;
            }
            minWeight = std::clamp(minWeight, 0.0F, 1.0F);
        } else if (arg == "--sequence-min-length") {
            if (!parseFloat(sequenceMinLength, "--sequence-min-length")) {
                return 1;
            }
            sequenceMinLength = std::max(0.0F, sequenceMinLength);
        } else if (arg == "--sequence-min-brightness") {
            if (!parseFloat(sequenceMinBrightness, "--sequence-min-brightness")) {
                return 1;
            }
            sequenceMinBrightness = std::max(0.0F, sequenceMinBrightness);
        } else if (arg == "--recurrence-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--recurrence-threshold requires a non-negative integer");
                return 1;
            }
            try {
                recurrenceThreshold = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--recurrence-threshold must be a non-negative integer");
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (!parseFloat(options.sigmaThreshold, "--sigma-threshold")) {
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (!parseFloat(options.minPeak, "--min-peak")) {
                return 1;
            }
        } else if (arg == "--min-length") {
            if (!parseFloat(options.minLength, "--min-length")) {
                return 1;
            }
        } else if (arg == "--airplane-length") {
            if (!parseFloat(options.airplaneLength, "--airplane-length")) {
                return 1;
            }
        } else if (arg == "--max-width") {
            if (!parseFloat(options.maxWidth, "--max-width")) {
                return 1;
            }
        } else if (arg == "--mask-radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--mask-radius requires a positive integer");
                return 1;
            }
            try {
                options.maskRadius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--mask-radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--inpaint-radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--inpaint-radius requires a positive integer");
                return 1;
            }
            try {
                options.inpaintRadius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--inpaint-radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--preserve-meteors") {
            if (!parseOnOff(args, i, "--preserve-meteors", options.preserveMeteors)) {
                return 1;
            }
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, readOptions.raw)) {
                return 1;
            }
        } else if (arg == "--remove-kinds") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--remove-kinds requires a comma-separated list");
                return 1;
            }
            options.removeAirplanes = false;
            options.removeDrones = false;
            options.removeSatellites = false;
            options.removeMeteors = false;
            for (const auto& kind : splitCommaList(args[++i])) {
                if (kind == "airplane" || kind == "airplanes") {
                    options.removeAirplanes = true;
                } else if (kind == "drone" || kind == "drones") {
                    options.removeDrones = true;
                } else if (kind == "satellite" || kind == "satellites") {
                    options.removeSatellites = true;
                } else if (kind == "meteor" || kind == "meteors") {
                    options.removeMeteors = true;
                    options.preserveMeteors = false;
                } else {
                    printError("ArgumentInvalid", "--remove-kinds supports airplane, drone, satellite, and meteor");
                    return 1;
                }
            }
        } else {
            inputFiles.emplace_back(arg);
        }
    }

    if (!inputDirectory.has_value() && inputFiles.empty()) {
        printError("ArgumentMissing", "artifacts clean-sequence requires --input <directory> or image paths");
        return 1;
    }
    if (!outputDirectory.has_value()) {
        printError("ArgumentMissing", "artifacts clean-sequence requires --output-dir <directory>");
        return 1;
    }
    if (inputDirectory.has_value()) {
        if (!std::filesystem::is_directory(*inputDirectory)) {
            printError("InputPathInvalid", "--input must point to a directory");
            return 1;
        }
        auto directoryInputs = collectDirectoryInputs(*inputDirectory);
        inputFiles.insert(inputFiles.end(), directoryInputs.begin(), directoryInputs.end());
    }
    if (inputFiles.empty()) {
        printError("InputMissing", "No input images were provided");
        return 1;
    }

    try {
        std::filesystem::create_directories(*outputDirectory);
    } catch (const std::exception& error) {
        printError("OutputPathInvalid", std::string("Could not create output directory: ") + error.what());
        return 1;
    }

    const auto outputExtension = outputFormat == "png" ? std::string(".png") : std::string(".tiff");
    std::vector<std::string> usedOutputPaths;
    auto makeOutputPath = [&](const std::filesystem::path& source) {
        const std::string stem = source.stem().empty() ? std::string("frame") : source.stem().string();
        auto candidate = *outputDirectory / (prefix + stem + outputExtension);
        int suffix = 2;
        while (std::find(usedOutputPaths.begin(), usedOutputPaths.end(), candidate.string()) != usedOutputPaths.end()) {
            candidate = *outputDirectory / (prefix + stem + "-" + std::to_string(suffix++) + outputExtension);
        }
        usedOutputPaths.push_back(candidate.string());
        return candidate;
    };

    auto centerX = [](const photonstack::ArtifactTrail& trail) {
        return (trail.x1 + trail.x2) * 0.5F;
    };
    auto centerY = [](const photonstack::ArtifactTrail& trail) {
        return (trail.y1 + trail.y2) * 0.5F;
    };
    auto angleDelta = [](float a, float b) {
        constexpr float pi = 3.14159265358979323846F;
        float delta = std::fabs(a - b);
        while (delta > pi) {
            delta -= pi;
        }
        return std::min(delta, pi - delta);
    };
    auto overlap = [&](const photonstack::ArtifactTrail& a, const photonstack::ArtifactTrail& b) {
        if (angleDelta(a.angleRadians, b.angleRadians) > 0.18F) {
            return false;
        }
        const float unitX = std::cos(a.angleRadians);
        const float unitY = std::sin(a.angleRadians);
        const float normalX = -unitY;
        const float normalY = unitX;
        const float normalDistance = std::fabs((centerX(a) - centerX(b)) * normalX + (centerY(a) - centerY(b)) * normalY);
        if (normalDistance > std::max(8.0F, (a.width + b.width) * 1.6F)) {
            return false;
        }
        const float a1 = a.x1 * unitX + a.y1 * unitY;
        const float a2 = a.x2 * unitX + a.y2 * unitY;
        const float b1 = b.x1 * unitX + b.y1 * unitY;
        const float b2 = b.x2 * unitX + b.y2 * unitY;
        const float left = std::max(std::min(a1, a2), std::min(b1, b2));
        const float right = std::min(std::max(a1, a2), std::max(b1, b2));
        return std::max(0.0F, right - left) >= std::min(a.length, b.length) * 0.40F;
    };
    auto removableKind = [&](photonstack::ArtifactTrailKind kind) {
        switch (kind) {
        case photonstack::ArtifactTrailKind::Airplane:
            return options.removeAirplanes;
        case photonstack::ArtifactTrailKind::Drone:
            return options.removeDrones;
        case photonstack::ArtifactTrailKind::Satellite:
            return options.removeSatellites;
        case photonstack::ArtifactTrailKind::Meteor:
            return options.removeMeteors && !options.preserveMeteors;
        case photonstack::ArtifactTrailKind::Unknown:
            return false;
        }
        return false;
    };

    const photonstack::ImageCodec codec;
    const photonstack::ArtifactTrailRemover remover;
    std::vector<FrameSummary> frames(inputFiles.size());
    std::vector<std::vector<photonstack::ArtifactTrail>> detectedTrails(inputFiles.size());
    std::vector<std::vector<std::uint8_t>> recurrentMask(inputFiles.size());
    std::size_t failedFrames = 0;
    std::size_t totalTrails = 0;
    const std::size_t totalSteps = inputFiles.size() * 2;

    for (std::size_t frameIndex = 0; frameIndex < inputFiles.size(); ++frameIndex) {
        auto& frame = frames[frameIndex];
        frame.input = inputFiles[frameIndex].string();
        frame.output = makeOutputPath(inputFiles[frameIndex]).string();

        const auto readResult = codec.read(inputFiles[frameIndex], readOptions);
        if (!readResult.ok) {
            frame.errorCode = readResult.errorCode;
            frame.message = readResult.message;
            ++failedFrames;
            printProgressEvent("clean-sequence", "artifacts clean-sequence", "read-failed",
                               static_cast<double>(frameIndex + 1) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                               frameIndex + 1, totalSteps);
            continue;
        }

        const auto detectResult = remover.detect(readResult.image, options);
        if (!detectResult.ok) {
            frame.errorCode = detectResult.errorCode;
            frame.message = detectResult.message;
            ++failedFrames;
            printProgressEvent("clean-sequence", "artifacts clean-sequence", "detect-failed",
                               static_cast<double>(frameIndex + 1) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                               frameIndex + 1, totalSteps);
            continue;
        }

        detectedTrails[frameIndex] = detectResult.trails;
        recurrentMask[frameIndex].assign(detectResult.trails.size(), 0);
        frame.trails = detectResult.trails.size();
        frame.protectedMeteors = detectResult.protectedMeteors;
        totalTrails += frame.trails;
        printProgressEvent("clean-sequence", "artifacts clean-sequence", "detected",
                           static_cast<double>(frameIndex + 1) / static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                           frameIndex + 1, totalSteps);
    }

    for (std::size_t frameIndex = 0; frameIndex < detectedTrails.size(); ++frameIndex) {
        for (std::size_t trailIndex = 0; trailIndex < detectedTrails[frameIndex].size(); ++trailIndex) {
            const auto& trail = detectedTrails[frameIndex][trailIndex];
            if (!removableKind(trail.kind) || trail.weight < minWeight || trail.length < sequenceMinLength ||
                trail.meanBrightness < sequenceMinBrightness) {
                continue;
            }

            std::size_t recurringFrames = 0;
            if (recurrenceThreshold > 0) {
                for (std::size_t otherFrame = 0; otherFrame < detectedTrails.size(); ++otherFrame) {
                    if (otherFrame == frameIndex) {
                        continue;
                    }
                    const auto repeated = std::any_of(
                        detectedTrails[otherFrame].begin(),
                        detectedTrails[otherFrame].end(),
                        [&](const photonstack::ArtifactTrail& otherTrail) {
                            return otherTrail.kind == trail.kind && overlap(trail, otherTrail);
                        });
                    if (repeated) {
                        ++recurringFrames;
                        if (recurringFrames >= recurrenceThreshold) {
                            break;
                        }
                    }
                }
            }

            if (recurrenceThreshold > 0 && recurringFrames >= recurrenceThreshold) {
                recurrentMask[frameIndex][trailIndex] = 1;
                frames[frameIndex].recurrentTrails += 1;
                continue;
            }
            frames[frameIndex].selectedIndices.push_back(trailIndex);
        }
    }

    std::size_t cleanedFrames = 0;
    std::size_t removedTrails = 0;
    for (std::size_t frameIndex = 0; frameIndex < inputFiles.size(); ++frameIndex) {
        auto& frame = frames[frameIndex];
        if (!frame.errorCode.empty()) {
            continue;
        }

        const auto readResult = codec.read(inputFiles[frameIndex], readOptions);
        if (!readResult.ok) {
            frame.errorCode = readResult.errorCode;
            frame.message = readResult.message;
            ++failedFrames;
            continue;
        }

        photonstack::ImageBuffer outputImage = readResult.image;
        if (!frame.selectedIndices.empty()) {
            auto removeOptions = options;
            removeOptions.selectedIndices = frame.selectedIndices;
            const auto removeResult = remover.remove(readResult.image, removeOptions);
            if (!removeResult.ok) {
                frame.errorCode = removeResult.errorCode;
                frame.message = removeResult.message;
                ++failedFrames;
                continue;
            }
            outputImage = removeResult.image;
            frame.removedTrails = removeResult.removedTrails;
            frame.protectedMeteors = removeResult.protectedMeteors;
            removedTrails += frame.removedTrails;
            if (frame.removedTrails > 0) {
                ++cleanedFrames;
            }
        }

        const auto writeResult = codec.write(outputImage, frame.output);
        frame.ok = writeResult.ok;
        frame.errorCode = writeResult.errorCode;
        frame.message = writeResult.ok ? (frame.selectedIndices.empty() ? "Copied unchanged frame" : "Cleaned transient trails")
                                       : writeResult.message;
        if (!writeResult.ok) {
            ++failedFrames;
        }
        printProgressEvent("clean-sequence", "artifacts clean-sequence", writeResult.ok ? "written" : "write-failed",
                           static_cast<double>(inputFiles.size() + frameIndex + 1) /
                               static_cast<double>(std::max<std::size_t>(totalSteps, 1)),
                           inputFiles.size() + frameIndex + 1, totalSteps);
    }

    const auto selectedTotal = std::accumulate(frames.begin(), frames.end(), std::size_t{0},
                                              [](std::size_t sum, const FrameSummary& frame) {
                                                  return sum + frame.selectedIndices.size();
                                              });
    const auto recurrentTotal = std::accumulate(frames.begin(), frames.end(), std::size_t{0},
                                               [](std::size_t sum, const FrameSummary& frame) {
                                                   return sum + frame.recurrentTrails;
                                               });

    std::cout << "{\n"
              << "  \"type\": \"" << (failedFrames == 0 ? "complete" : "partial") << "\",\n"
              << "  \"command\": \"artifacts clean-sequence\",\n"
              << "  \"frames\": " << frames.size() << ",\n"
              << "  \"cleanedFrames\": " << cleanedFrames << ",\n"
              << "  \"failedFrames\": " << failedFrames << ",\n"
              << "  \"trails\": " << totalTrails << ",\n"
              << "  \"selectedTrails\": " << selectedTotal << ",\n"
              << "  \"removedTrails\": " << removedTrails << ",\n"
              << "  \"recurrentTrails\": " << recurrentTotal << ",\n"
              << "  \"minWeight\": " << minWeight << ",\n"
              << "  \"sequenceMinLength\": " << sequenceMinLength << ",\n"
              << "  \"sequenceMinBrightness\": " << sequenceMinBrightness << ",\n"
              << "  \"recurrenceThreshold\": " << recurrenceThreshold << ",\n"
              << "  \"outputFormat\": \"" << jsonEscape(outputFormat) << "\",\n"
              << "  \"outputDirectory\": \"" << jsonEscape(outputDirectory->string()) << "\",\n"
              << "  \"items\": [\n";
    for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex) {
        const auto& frame = frames[frameIndex];
        std::cout << "    {\"ok\":" << (frame.ok ? "true" : "false")
                  << ",\"input\":\"" << jsonEscape(frame.input) << "\""
                  << ",\"output\":\"" << jsonEscape(frame.output) << "\""
                  << ",\"trails\":" << frame.trails
                  << ",\"removedTrails\":" << frame.removedTrails
                  << ",\"protectedMeteors\":" << frame.protectedMeteors
                  << ",\"recurrentTrails\":" << frame.recurrentTrails
                  << ",\"selectedIndices\":[";
        for (std::size_t index = 0; index < frame.selectedIndices.size(); ++index) {
            std::cout << frame.selectedIndices[index];
            if (index + 1 < frame.selectedIndices.size()) {
                std::cout << ",";
            }
        }
        std::cout << "]"
                  << ",\"errorCode\":\"" << jsonEscape(frame.errorCode) << "\""
                  << ",\"message\":\"" << jsonEscape(frame.message) << "\"}";
        if (frameIndex + 1 < frames.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
    return failedFrames == 0 ? 0 : 1;
}

int processMeteors(const std::string& mode, const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> sourcePath;
    std::optional<std::filesystem::path> basePath;
    std::optional<std::filesystem::path> layerPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::MeteorLayerOptions options;
    photonstack::ImageReadOptions readOptions;
    readOptions.fits.maskNonFinitePixels = true;
    readOptions.raw.linearOutput = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };

        if (arg == "--input" || arg == "--source") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", arg + " requires an image path");
                return 1;
            }
            sourcePath = args[++i];
            inputPath = sourcePath;
        } else if (arg == "--base") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--base requires an image path");
                return 1;
            }
            basePath = args[++i];
        } else if (arg == "--layer") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--layer requires an image path");
                return 1;
            }
            layerPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--min-confidence") {
            if (!parseFloat(options.minConfidence, "--min-confidence")) {
                return 1;
            }
        } else if (arg == "--mask-radius") {
            if (!parseFloat(options.maskRadius, "--mask-radius")) {
                return 1;
            }
        } else if (arg == "--feather-radius") {
            if (!parseFloat(options.featherRadius, "--feather-radius")) {
                return 1;
            }
        } else if (arg == "--opacity") {
            if (!parseFloat(options.opacity, "--opacity")) {
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (!parseFloat(options.detection.sigmaThreshold, "--sigma-threshold")) {
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (!parseFloat(options.detection.minPeak, "--min-peak")) {
                return 1;
            }
        } else if (arg == "--min-length") {
            if (!parseFloat(options.detection.minLength, "--min-length")) {
                return 1;
            }
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, readOptions.raw)) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown meteors argument: " + arg);
            return 1;
        }
    }

    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "meteors " + mode + " requires --output <image>");
        return 1;
    }
    if (mode == "extract" && !sourcePath.has_value()) {
        printError("ArgumentMissing", "meteors extract requires --input <image>");
        return 1;
    }
    if (mode == "restore" && (!basePath.has_value() || !sourcePath.has_value())) {
        printError("ArgumentMissing", "meteors restore requires --base <stacked> and --source <image>");
        return 1;
    }
    if (mode == "compose" && (!basePath.has_value() || !layerPath.has_value())) {
        printError("ArgumentMissing", "meteors compose requires --base <stacked> and --layer <png>");
        return 1;
    }

    const bool scientificOutput = photonstack::isFitsPath(*outputPath);
    if (scientificOutput) {
        const bool compatibleScientificInputs =
            (mode == "extract" && sourcePath.has_value() && photonstack::isFitsPath(*sourcePath)) ||
            (mode == "restore" && basePath.has_value() && sourcePath.has_value() &&
             photonstack::isFitsPath(*basePath) && photonstack::isFitsPath(*sourcePath)) ||
            (mode == "compose" && basePath.has_value() && layerPath.has_value() &&
             photonstack::isFitsPath(*basePath) && photonstack::isFitsPath(*layerPath));
        if (!compatibleScientificInputs) {
            printError(
                "ScientificBridgeUnsupported",
                "Scientific meteor output requires FITS source, base, and layer inputs used by the selected mode"
            );
            return 1;
        }
    }

    const photonstack::ImageCodec codec;
    const photonstack::MeteorLayerComposer composer;
    photonstack::MeteorLayerResult result;

    if (mode == "extract") {
        const auto detectionRead = codec.read(*sourcePath, readOptions);
        if (!detectionRead.ok) {
            printError(detectionRead.errorCode, detectionRead.message);
            return 1;
        }
        if (scientificOutput) {
            const auto sourceRead = codec.read(*sourcePath, scientificFitsReadOptions());
            if (!sourceRead.ok) {
                printError(sourceRead.errorCode, sourceRead.message);
                return 1;
            }
            result = composer.extractUsingDetectionImage(sourceRead.image, detectionRead.image, options);
        } else {
            result = composer.extract(detectionRead.image, options);
        }
        if (result.ok) {
            const auto writeResult = codec.write(result.layer, *outputPath);
            if (!writeResult.ok) {
                printError(writeResult.errorCode, writeResult.message);
                return 1;
            }
        }
    } else if (mode == "restore") {
        const auto sourceDetectionRead = codec.read(*sourcePath, readOptions);
        if (!sourceDetectionRead.ok) {
            printError(sourceDetectionRead.errorCode, sourceDetectionRead.message);
            return 1;
        }
        const auto effectiveReadOptions = scientificOutput ? scientificFitsReadOptions() : readOptions;
        const auto baseRead = codec.read(*basePath, effectiveReadOptions);
        if (!baseRead.ok) {
            printError(baseRead.errorCode, baseRead.message);
            return 1;
        }
        if (scientificOutput) {
            const auto sourceRead = codec.read(*sourcePath, effectiveReadOptions);
            if (!sourceRead.ok) {
                printError(sourceRead.errorCode, sourceRead.message);
                return 1;
            }
            result = composer.restoreUsingDetectionImage(
                baseRead.image,
                sourceRead.image,
                sourceDetectionRead.image,
                options
            );
        } else {
            result = composer.restore(baseRead.image, sourceDetectionRead.image, options);
        }
        if (result.ok) {
            const auto writeResult = codec.write(result.image, *outputPath);
            if (!writeResult.ok) {
                printError(writeResult.errorCode, writeResult.message);
                return 1;
            }
        }
    } else if (mode == "compose") {
        const auto effectiveReadOptions = scientificOutput ? scientificFitsReadOptions() : readOptions;
        const auto baseRead = codec.read(*basePath, effectiveReadOptions);
        if (!baseRead.ok) {
            printError(baseRead.errorCode, baseRead.message);
            return 1;
        }
        const auto layerRead = codec.read(*layerPath, effectiveReadOptions);
        if (!layerRead.ok) {
            printError(layerRead.errorCode, layerRead.message);
            return 1;
        }
        result = composer.compose(baseRead.image, layerRead.image, options);
        if (result.ok) {
            const auto writeResult = codec.write(result.image, *outputPath);
            if (!writeResult.ok) {
                printError(writeResult.errorCode, writeResult.message);
                return 1;
            }
        }
    } else {
        printError("ArgumentInvalid", "Usage: photonstack meteors extract|restore|compose ...");
        return 1;
    }

    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"meteors " << jsonEscape(mode) << "\",\n"
              << "  \"valueDomain\": \"" << (scientificOutput ? "scientific-mask-transfer" : "display") << "\",\n"
              << "  \"meteors\": " << result.meteors.size() << ",\n"
              << "  \"restoredMeteors\": " << result.restoredMeteors << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\",\n"
              << "  \"items\": [\n";
    for (std::size_t i = 0; i < result.meteors.size(); ++i) {
        const auto& trail = result.meteors[i];
        std::cout << "    {\"confidence\":" << trail.confidence << ","
                  << "\"x1\":" << trail.x1 << ","
                  << "\"y1\":" << trail.y1 << ","
                  << "\"x2\":" << trail.x2 << ","
                  << "\"y2\":" << trail.y2 << ","
                  << "\"length\":" << trail.length << ","
                  << "\"width\":" << trail.width << ","
                  << "\"meanBrightness\":" << trail.meanBrightness << ","
                  << "\"weight\":" << trail.weight << "}";
        if (i + 1 < result.meteors.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
    (void)inputPath;
    return 0;
}

int processClouds(const std::string& mode, const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    std::vector<std::filesystem::path> temporalReferences;
    photonstack::CloudRemovalOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };
        auto parseUInt = [&](std::uint32_t& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a positive integer");
                return false;
            }
            try {
                target = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a positive integer");
                return false;
            }
            return true;
        };

        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--reference") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--reference requires an image path");
                return 1;
            }
            temporalReferences.emplace_back(args[++i]);
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--grid-x") {
            if (!parseUInt(options.columns, "--grid-x")) {
                return 1;
            }
        } else if (arg == "--grid-y") {
            if (!parseUInt(options.rows, "--grid-y")) {
                return 1;
            }
        } else if (arg == "--min-brightness-delta") {
            if (!parseFloat(options.minBrightnessDelta, "--min-brightness-delta")) {
                return 1;
            }
        } else if (arg == "--min-coverage") {
            if (!parseFloat(options.minCoverage, "--min-coverage")) {
                return 1;
            }
        } else if (arg == "--strength") {
            if (!parseFloat(options.strength, "--strength")) {
                return 1;
            }
        } else if (arg == "--feather-radius") {
            if (!parseFloat(options.featherRadius, "--feather-radius")) {
                return 1;
            }
        } else if (arg == "--selected-indices") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--selected-indices requires a comma-separated list");
                return 1;
            }
            try {
                for (const auto& token : splitCommaList(args[++i])) {
                    options.selectedIndices.push_back(parseUnsignedInteger<std::size_t>(token));
                }
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--selected-indices must contain zero-based integer indices");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown clouds argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "clouds " + mode + " requires --input <image>");
        return 1;
    }
    const bool temporalMode = mode == "temporal-detect" || mode == "temporal-remove";
    const bool removalMode = mode == "remove" || mode == "temporal-remove";
    if (removalMode && !outputPath.has_value()) {
        printError("ArgumentMissing", "clouds " + mode + " requires --output <image>");
        return 1;
    }
    if (temporalMode && temporalReferences.size() < 2) {
        printError("ArgumentMissing", "clouds " + mode + " requires at least two --reference images");
        return 1;
    }
    const bool scientificBridge = removalMode && photonstack::isFitsPath(*outputPath);
    if (scientificBridge && !photonstack::isFitsPath(*inputPath)) {
        printError(
            "ScientificBridgeUnsupported",
            "Scientific cloud removal requires a FITS input when the output is FITS"
        );
        return 1;
    }

    const photonstack::ImageCodec codec;
    photonstack::ImageReadOptions displayReadOptions;
    displayReadOptions.fits.maskNonFinitePixels = scientificBridge;
    const std::string progressCommand = "clouds " + mode;
    printProgressEvent("clouds", progressCommand, "read-input", 0.04);
    const auto readResult = codec.read(*inputPath, displayReadOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }
    std::optional<photonstack::ImageBuffer> scientificSource;
    if (scientificBridge) {
        const auto scientificRead = codec.read(*inputPath, scientificFitsReadOptions());
        if (!scientificRead.ok) {
            printError(scientificRead.errorCode, scientificRead.message);
            return 1;
        }
        scientificSource = scientificRead.image;
    }

    const photonstack::CloudRemoval clouds;
    std::vector<photonstack::CloudRemovalResult> temporalDetections;
    if (temporalMode) {
        photonstack::CloudRemovalOptions weakTemporalOptions = options;
        weakTemporalOptions.minBrightnessDelta = std::min(weakTemporalOptions.minBrightnessDelta, 0.015F);
        temporalDetections.reserve(temporalReferences.size());
        for (std::size_t referenceIndex = 0; referenceIndex < temporalReferences.size(); ++referenceIndex) {
            const auto& reference = temporalReferences[referenceIndex];
            const double beforeReference = 0.08 + 0.62 * static_cast<double>(referenceIndex) /
                                                     static_cast<double>(temporalReferences.size());
            printProgressEvent(
                "clouds",
                progressCommand,
                "read-reference",
                beforeReference,
                referenceIndex + 1,
                temporalReferences.size()
            );
            const auto referenceRead = codec.read(reference, displayReadOptions);
            if (!referenceRead.ok) {
                printError(referenceRead.errorCode, referenceRead.message);
                return 1;
            }
            if (referenceRead.image.width != readResult.image.width || referenceRead.image.height != readResult.image.height ||
                referenceRead.image.channels != readResult.image.channels ||
                referenceRead.image.colorEncoding != readResult.image.colorEncoding) {
                printError(
                    "ImageDimensionMismatch",
                    "Temporal cloud detection requires reference images with matching dimensions, channels, and color encoding"
                );
                return 1;
            }
            const auto detection = clouds.detect(referenceRead.image, weakTemporalOptions);
            if (!detection.ok) {
                printError(detection.errorCode, detection.message);
                return 1;
            }
            temporalDetections.push_back(detection);
            const double afterReference = 0.08 + 0.62 * static_cast<double>(referenceIndex + 1) /
                                                    static_cast<double>(temporalReferences.size());
            printProgressEvent(
                "clouds",
                progressCommand,
                "detected-reference",
                afterReference,
                referenceIndex + 1,
                temporalReferences.size()
            );
        }
    }
    printProgressEvent("clouds", progressCommand, temporalMode ? "confirm-temporal" : "detect", 0.76);
    auto result = mode == "remove" ? clouds.remove(readResult.image, options)
                                    : mode == "temporal-remove"
                                          ? clouds.removeTemporalFromDetections(readResult.image, temporalDetections, options)
                                          : mode == "temporal-detect"
                                                ? clouds.detectTemporalFromDetections(readResult.image, temporalDetections, options)
                                                : clouds.detect(readResult.image, options);
    if (!result.ok) {
        printError(result.errorCode, result.message);
        return 1;
    }
    printProgressEvent("clouds", progressCommand, removalMode ? "prepare-output" : "detected", 0.88);
    std::size_t bridgedSamples = 0;
    if (removalMode) {
        if (scientificBridge) {
            const auto bridgeResult = photonstack::ScientificDisplayBridge().applyCorrections(
                *scientificSource,
                readResult.image,
                result.image
            );
            if (!bridgeResult.ok) {
                printError(bridgeResult.errorCode, bridgeResult.message);
                return 1;
            }
            result.image = bridgeResult.image;
            bridgedSamples = bridgeResult.correctedSamples;
        }
        printProgressEvent("clouds", progressCommand, "write", 0.93);
        const auto writeResult = codec.write(result.image, *outputPath);
        if (!writeResult.ok) {
            printError(writeResult.errorCode, writeResult.message);
            return 1;
        }
        printProgressEvent("clouds", progressCommand, "written", 0.99);
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"clouds " << jsonEscape(mode) << "\",\n"
              << "  \"valueDomain\": \"" << (scientificBridge ? "scientific-bridge" : "display") << "\",\n"
              << "  \"bridgedSamples\": " << bridgedSamples << ",\n"
              << "  \"width\": " << readResult.image.width << ",\n"
              << "  \"height\": " << readResult.image.height << ",\n"
              << "  \"clouds\": " << result.regions.size() << ",\n"
              << "  \"removedClouds\": " << result.removedRegions << ",\n"
              << "  \"temporalFramesUsed\": " << result.temporalFramesUsed << ",\n"
              << "  \"temporallyConfirmedClouds\": " << result.temporallyConfirmedRegions << ",\n"
              << "  \"items\": [\n";
    for (std::size_t i = 0; i < result.regions.size(); ++i) {
        const auto& region = result.regions[i];
        std::cout << "    {\"confidence\":" << region.confidence << ","
                  << "\"x\":" << region.x << ","
                  << "\"y\":" << region.y << ","
                  << "\"width\":" << region.width << ","
                  << "\"height\":" << region.height << ","
                  << "\"coverage\":" << region.coverage << ","
                  << "\"meanLuminance\":" << region.meanLuminance << ","
                  << "\"backgroundLuminance\":" << region.backgroundLuminance << ","
                  << "\"temporalSupport\":" << region.temporalSupport << ","
                  << "\"mask\":[";
        for (std::size_t maskIndex = 0; maskIndex < region.maskRects.size(); ++maskIndex) {
            const auto& mask = region.maskRects[maskIndex];
            std::cout << "{\"x\":" << mask.x << ","
                      << "\"y\":" << mask.y << ","
                      << "\"width\":" << mask.width << ","
                      << "\"height\":" << mask.height << "}";
            if (maskIndex + 1 < region.maskRects.size()) {
                std::cout << ",";
            }
        }
        std::cout << "]}";
        if (i + 1 < result.regions.size()) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    std::cout << "  ]";
    if (outputPath.has_value()) {
        std::cout << ",\n  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n";
    } else {
        std::cout << "\n";
    }
    std::cout << "}\n";
    return 0;
}

int detectStars(const std::vector<std::string>& args) {
    if (args.empty()) {
        printError("ArgumentInvalid", "Usage: photonstack stars detect --input <image> [--sigma-threshold 3.0] [--min-peak 0.05] [--max-stars 50000] [--summary-only]");
        return 1;
    }

    std::optional<std::filesystem::path> inputPath;
    photonstack::StarDetectionOptions options;
    options.maxStars = 50000;
    bool summaryOnly = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                options.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--min-peak requires a positive number");
                return 1;
            }
            try {
                options.minPeak = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--min-peak must be a positive number");
                return 1;
            }
        } else if (arg == "--max-stars") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--max-stars requires a positive integer");
                return 1;
            }
            try {
                options.maxStars = parseUnsignedInteger<std::size_t>(args[++i]);
                if (options.maxStars == 0) {
                    throw std::invalid_argument("zero max-stars");
                }
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--max-stars must be a positive integer");
                return 1;
            }
        } else if (arg == "--summary-only") {
            summaryOnly = true;
        } else {
            printError("ArgumentInvalid", "Unknown stars detect argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "stars detect requires --input <image>");
        return 1;
    }

    photonstack::ImageReadOptions readOptions;
    readOptions.raw.linearOutput = false;
    readOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, readOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::StarDetector detector;
    const auto detectionResult = detector.detect(readResult.image, options);
    if (!detectionResult.ok) {
        printError(detectionResult.errorCode, detectionResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"stars detect\",\n"
              << "  \"valueDomain\": \"display\",\n"
              << "  \"count\": " << detectionResult.stars.size() << ",\n"
              << "  \"limited\": " << (detectionResult.stars.size() >= options.maxStars ? "true" : "false");
    if (summaryOnly) {
        std::cout << "\n}\n";
        return 0;
    }
    std::cout << ",\n  \"stars\": [\n";
    for (std::size_t i = 0; i < detectionResult.stars.size(); ++i) {
        const auto& star = detectionResult.stars[i];
        std::cout << "    {\"x\": " << star.x << ", \"y\": " << star.y << ", \"flux\": " << star.flux
                  << ", \"peak\": " << star.peak << ", \"fwhm\": " << star.fwhm
                  << ", \"eccentricity\": " << star.eccentricity << "}";
        if (i + 1 < detectionResult.stars.size()) {
            std::cout << ',';
        }
        std::cout << '\n';
    }
    std::cout << "  ]\n"
              << "}\n";
    return 0;
}

int createStarMask(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::StarMaskOptions options;
    options.detection.maxStars = 50000;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--layered") {
            options.layered = true;
        } else if (arg == "--large-radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--large-radius requires a positive integer");
                return 1;
            }
            try {
                options.largeRadius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--large-radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                options.detection.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--min-peak requires a positive number");
                return 1;
            }
            try {
                options.detection.minPeak = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--min-peak must be a positive number");
                return 1;
            }
        } else if (arg == "--max-stars") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--max-stars requires a positive integer");
                return 1;
            }
            try {
                options.detection.maxStars = parseUnsignedInteger<std::size_t>(args[++i]);
                if (options.detection.maxStars == 0) {
                    throw std::invalid_argument("zero max-stars");
                }
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--max-stars must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown stars mask argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "stars mask requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "stars mask requires --output <image>");
        return 1;
    }

    photonstack::ImageReadOptions readOptions;
    readOptions.raw.linearOutput = false;
    readOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, readOptions);
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const photonstack::StarMask starMask;
    const auto maskResult = starMask.create(readResult.image, options);
    if (!maskResult.ok) {
        printError(maskResult.errorCode, maskResult.message);
        return 1;
    }

    const auto writeResult = codec.write(maskResult.mask, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"stars mask\",\n"
              << "  \"valueDomain\": \"display-mask\",\n"
              << "  \"stars\": " << maskResult.stars << ",\n"
              << "  \"largeStars\": " << maskResult.largeStars << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int reduceStars(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::StarReductionOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--amount") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--amount requires a number between 0 and 1");
                return 1;
            }
            try {
                options.amount = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--amount must be a number between 0 and 1");
                return 1;
            }
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--profile-aware") {
            options.profileAware = true;
            options.mask.layered = true;
        } else if (arg == "--edge-aware") {
            if (!parseOnOff(args, i, "--edge-aware", options.edgeAware)) {
                return 1;
            }
        } else if (arg == "--large-radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--large-radius requires a positive integer");
                return 1;
            }
            try {
                options.mask.largeRadius = parseUnsignedInteger<std::uint32_t>(args[++i]);
                options.mask.layered = true;
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--large-radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--sigma-threshold requires a positive number");
                return 1;
            }
            try {
                options.mask.detection.sigmaThreshold = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--sigma-threshold must be a positive number");
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--min-peak requires a positive number");
                return 1;
            }
            try {
                options.mask.detection.minPeak = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--min-peak must be a positive number");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown stars reduce argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "stars reduce requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "stars reduce requires --output <image>");
        return 1;
    }

    photonstack::ImageReadOptions detectionReadOptions;
    detectionReadOptions.raw.linearOutput = false;
    detectionReadOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto detectionRead = codec.read(*inputPath, detectionReadOptions);
    if (!detectionRead.ok) {
        printError(detectionRead.errorCode, detectionRead.message);
        return 1;
    }
    const auto sourceRead = codec.read(*inputPath, scientificFitsReadOptions());
    if (!sourceRead.ok) {
        printError(sourceRead.errorCode, sourceRead.message);
        return 1;
    }

    const photonstack::StarReducer reducer;
    const auto reductionResult = reducer.reduceUsingDetectionImage(sourceRead.image, detectionRead.image, options);
    if (!reductionResult.ok) {
        printError(reductionResult.errorCode, reductionResult.message);
        return 1;
    }

    const auto writeResult = codec.write(reductionResult.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"stars reduce\",\n"
              << "  \"stars\": " << reductionResult.stars << ",\n"
              << "  \"edgeAware\": " << (options.edgeAware ? "true" : "false") << ",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"detectionDomain\": \"display\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int reduceComa(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::ComaReductionOptions options;
    options.detection.maxStars = 8000;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseFloat = [&](float& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a number");
                return false;
            }
            try {
                target = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a number");
                return false;
            }
            return true;
        };
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--amount") {
            if (!parseFloat(options.amount, "--amount")) {
                return 1;
            }
        } else if (arg == "--radius") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--radius requires a positive integer");
                return 1;
            }
            try {
                options.radius = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--radius must be a positive integer");
                return 1;
            }
        } else if (arg == "--eccentricity") {
            if (!parseFloat(options.eccentricityThreshold, "--eccentricity")) {
                return 1;
            }
        } else if (arg == "--edge-aware") {
            if (!parseOnOff(args, i, "--edge-aware", options.edgeAware)) {
                return 1;
            }
        } else if (arg == "--highlight-protection") {
            if (!parseFloat(options.highlightProtection, "--highlight-protection")) {
                return 1;
            }
        } else if (arg == "--sigma-threshold") {
            if (!parseFloat(options.detection.sigmaThreshold, "--sigma-threshold")) {
                return 1;
            }
        } else if (arg == "--min-peak") {
            if (!parseFloat(options.detection.minPeak, "--min-peak")) {
                return 1;
            }
        } else if (arg == "--max-stars") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--max-stars requires a positive integer");
                return 1;
            }
            try {
                options.detection.maxStars = parseUnsignedInteger<std::size_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--max-stars must be a positive integer");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown stars coma argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "stars coma requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "stars coma requires --output <image>");
        return 1;
    }

    photonstack::ImageReadOptions detectionReadOptions;
    detectionReadOptions.raw.linearOutput = false;
    detectionReadOptions.fits.maskNonFinitePixels = true;
    const photonstack::ImageCodec codec;
    const auto detectionRead = codec.read(*inputPath, detectionReadOptions);
    if (!detectionRead.ok) {
        printError(detectionRead.errorCode, detectionRead.message);
        return 1;
    }
    const auto sourceRead = codec.read(*inputPath, scientificFitsReadOptions());
    if (!sourceRead.ok) {
        printError(sourceRead.errorCode, sourceRead.message);
        return 1;
    }

    const photonstack::ComaReducer reducer;
    const auto reductionResult = reducer.reduceUsingDetectionImage(sourceRead.image, detectionRead.image, options);
    if (!reductionResult.ok) {
        printError(reductionResult.errorCode, reductionResult.message);
        return 1;
    }

    const auto writeResult = codec.write(reductionResult.image, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"stars coma\",\n"
              << "  \"stars\": " << reductionResult.stars << ",\n"
              << "  \"correctedStars\": " << reductionResult.correctedStars << ",\n"
              << "  \"averageEccentricity\": " << reductionResult.averageEccentricity << ",\n"
              << "  \"fitsValues\": \"scientific\",\n"
              << "  \"detectionDomain\": \"display\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int previewImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    photonstack::ImageReadOptions readOptions;
    std::uint32_t width = 1600;
    photonstack::CurvesOptions curveOptions;
    bool applyCurve = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--width") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--width requires a positive integer");
                return 1;
            }
            try {
                width = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--width must be a positive integer");
                return 1;
            }
        } else if (arg == "--curve-points") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--curve-points requires input:output pairs");
                return 1;
            }
            const auto parsed = parseCurvePoints(args[++i]);
            if (!parsed.has_value()) {
                printError("ArgumentInvalid", "--curve-points must look like 0:0,0.5:0.6,1:1");
                return 1;
            }
            curveOptions.points = parsed->points;
            applyCurve = true;
        } else if (arg == "--curve-channel") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--curve-channel requires rgb, red, green, blue, or luminance");
                return 1;
            }
            const auto parsed = parseCurveChannel(args[++i]);
            if (!parsed.has_value()) {
                printError("ArgumentInvalid", "--curve-channel must be rgb, red, green, blue, or luminance");
                return 1;
            }
            curveOptions.channel = *parsed;
            applyCurve = true;
        } else if (arg.rfind("--raw-", 0) == 0) {
            if (!parseRawDecodeArgument(args, i, readOptions.raw)) {
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown preview argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value()) {
        printError("ArgumentMissing", "preview requires --input <image>");
        return 1;
    }
    if (!outputPath.has_value()) {
        printError("ArgumentMissing", "preview requires --output <image>");
        return 1;
    }
    PhotonStackCurvePreviewRequest request = photonstack_default_curve_preview_request();
    request.width = width;
    request.rawWhiteBalanceMode = bridgeRawWhiteBalanceMode(readOptions.raw.whiteBalanceMode);
    request.rawManualWhiteBalanceTemperature = readOptions.raw.manualWhiteBalanceTemperature;
    request.rawManualWhiteBalanceTint = readOptions.raw.manualWhiteBalanceTint;
    request.rawExposureBias = readOptions.raw.exposureBias;
    request.rawBlackLevelMode = bridgeRawBlackLevelMode(readOptions.raw.blackLevelMode);
    request.rawManualBlackLevel = readOptions.raw.manualBlackLevel;
    request.rawDemosaicQuality = bridgeRawDemosaicQuality(readOptions.raw.demosaicQuality);
    request.rawLinearOutput = readOptions.raw.linearOutput ? 1 : 0;
    request.curveChannel = bridgeCurveChannel(curveOptions.channel);
    std::vector<PhotonStackCurvePoint> bridgeCurvePoints;
    if (applyCurve) {
        bridgeCurvePoints.reserve(curveOptions.points.size());
        for (const auto& point : curveOptions.points) {
            bridgeCurvePoints.push_back(PhotonStackCurvePoint{
                .input = point.input,
                .output = point.output,
            });
        }
        request.curvePoints = bridgeCurvePoints.data();
        request.curvePointCount = bridgeCurvePoints.size();
    }
    char fallbackErrorCode[256] = {};
    char fallbackErrorMessage[1024] = {};
    char errorBuffer[1024] = {};
    const auto result = photonstack_curve_preview_path(
        inputPath->string().c_str(),
        outputPath->string().c_str(),
        request,
        fallbackErrorCode,
        sizeof(fallbackErrorCode),
        fallbackErrorMessage,
        sizeof(fallbackErrorMessage),
        errorBuffer,
        sizeof(errorBuffer)
    );
    if (!result.ok) {
        printError("PreviewBridgeFailed", errorBuffer[0] == '\0' ? "preview bridge failed" : errorBuffer);
        return 1;
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"preview\",\n"
              << "  \"width\": " << result.width << ",\n"
              << "  \"height\": " << result.height << ",\n"
              << "  \"decodedWidth\": " << result.decodedWidth << ",\n"
              << "  \"decodedHeight\": " << result.decodedHeight << ",\n"
              << "  \"decodeBackend\": \"" << bridgeImageReadBackendName(result.backend) << "\",\n"
              << "  \"decodeFallback\": " << (result.usedFallback ? "true" : "false") << ",\n"
              << "  \"decodeFallbackCode\": \"" << jsonEscape(fallbackErrorCode) << "\",\n"
              << "  \"decodeFallbackMessage\": \"" << jsonEscape(fallbackErrorMessage) << "\",\n"
              << "  \"rawWhiteBalance\": \"" << rawWhiteBalanceName(readOptions.raw.whiteBalanceMode) << "\",\n"
              << "  \"rawTemperature\": " << readOptions.raw.manualWhiteBalanceTemperature << ",\n"
              << "  \"rawTint\": " << readOptions.raw.manualWhiteBalanceTint << ",\n"
              << "  \"rawExposureBias\": " << readOptions.raw.exposureBias << ",\n"
              << "  \"rawBlackLevel\": \"" << rawBlackLevelName(readOptions.raw.blackLevelMode) << "\",\n"
              << "  \"rawBlackValue\": " << readOptions.raw.manualBlackLevel << ",\n"
              << "  \"rawDemosaic\": \"" << rawDemosaicName(readOptions.raw.demosaicQuality) << "\",\n"
              << "  \"rawLinear\": " << (readOptions.raw.linearOutput ? "true" : "false") << ",\n"
              << "  \"curveApplied\": " << (applyCurve ? "true" : "false") << ",\n"
              << "  \"curveChannel\": \"" << curveChannelName(curveOptions.channel) << "\",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

int cropImage(const std::vector<std::string>& args) {
    std::optional<std::filesystem::path> inputPath;
    std::optional<std::filesystem::path> outputPath;
    std::optional<std::uint32_t> cropX;
    std::optional<std::uint32_t> cropY;
    std::optional<std::uint32_t> cropWidth;
    std::optional<std::uint32_t> cropHeight;
    std::optional<float> aspectRatio;
    float margin = 0.0F;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        auto parseUInt = [&](std::optional<std::uint32_t>& target, const std::string& name) -> bool {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", name + " requires a positive integer");
                return false;
            }
            try {
                target = parseUnsignedInteger<std::uint32_t>(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", name + " must be a positive integer");
                return false;
            }
            return true;
        };

        if (arg == "--input") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--input requires an image path");
                return 1;
            }
            inputPath = args[++i];
        } else if (arg == "--output") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--output requires an image path");
                return 1;
            }
            outputPath = args[++i];
        } else if (arg == "--x") {
            if (!parseUInt(cropX, "--x")) {
                return 1;
            }
        } else if (arg == "--y") {
            if (!parseUInt(cropY, "--y")) {
                return 1;
            }
        } else if (arg == "--width") {
            if (!parseUInt(cropWidth, "--width")) {
                return 1;
            }
        } else if (arg == "--height") {
            if (!parseUInt(cropHeight, "--height")) {
                return 1;
            }
        } else if (arg == "--aspect") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--aspect requires a value like 16:9");
                return 1;
            }
            const auto value = args[++i];
            const auto separator = value.find(':');
            if (separator == std::string::npos) {
                printError("ArgumentInvalid", "--aspect must look like 16:9");
                return 1;
            }
            try {
                const float aspectWidth = parseFiniteFloat(value.substr(0, separator));
                const float aspectHeight = parseFiniteFloat(value.substr(separator + 1));
                if (aspectWidth <= 0.0F || aspectHeight <= 0.0F) {
                    printError("ArgumentInvalid", "--aspect values must be positive");
                    return 1;
                }
                aspectRatio = aspectWidth / aspectHeight;
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--aspect must contain numeric values");
                return 1;
            }
        } else if (arg == "--margin") {
            if (i + 1 >= args.size()) {
                printError("ArgumentMissing", "--margin requires a number from 0 through 0.45");
                return 1;
            }
            try {
                margin = parseFiniteFloat(args[++i]);
            } catch (const std::exception&) {
                printError("ArgumentInvalid", "--margin must be a number from 0 through 0.45");
                return 1;
            }
            if (margin < 0.0F || margin > 0.45F) {
                printError("ArgumentInvalid", "--margin must be in [0, 0.45]");
                return 1;
            }
        } else {
            printError("ArgumentInvalid", "Unknown crop argument: " + arg);
            return 1;
        }
    }

    if (!inputPath.has_value() || !outputPath.has_value()) {
        printError("ArgumentMissing", "crop requires --input <image> and --output <image>");
        return 1;
    }

    const photonstack::ImageCodec codec;
    const auto readResult = codec.read(*inputPath, scientificFitsReadOptions());
    if (!readResult.ok) {
        printError(readResult.errorCode, readResult.message);
        return 1;
    }

    const auto& input = readResult.image;
    std::uint32_t x = cropX.value_or(0);
    std::uint32_t y = cropY.value_or(0);
    std::uint32_t width = cropWidth.value_or(input.width);
    std::uint32_t height = cropHeight.value_or(input.height);

    if (aspectRatio.has_value() && (!cropWidth.has_value() || !cropHeight.has_value())) {
        const auto requestedMarginX = static_cast<std::uint32_t>(
            std::round(static_cast<float>(input.width) * margin)
        );
        const auto requestedMarginY = static_cast<std::uint32_t>(
            std::round(static_cast<float>(input.height) * margin)
        );
        const auto marginX = std::min(requestedMarginX, (input.width - 1U) / 2U);
        const auto marginY = std::min(requestedMarginY, (input.height - 1U) / 2U);
        const std::uint32_t innerWidth = input.width - marginX * 2U;
        const std::uint32_t innerHeight = input.height - marginY * 2U;
        const float innerRatio = static_cast<float>(innerWidth) / static_cast<float>(innerHeight);
        if (innerRatio > *aspectRatio) {
            height = innerHeight;
            width = static_cast<std::uint32_t>(std::round(static_cast<float>(height) * *aspectRatio));
            x = marginX + (innerWidth - width) / 2U;
            y = marginY;
        } else {
            width = innerWidth;
            height = static_cast<std::uint32_t>(std::round(static_cast<float>(width) / *aspectRatio));
            x = marginX;
            y = marginY + (innerHeight - height) / 2U;
        }
    }

    if (width == 0 || height == 0 || x >= input.width || y >= input.height ||
        width > input.width - x || height > input.height - y) {
        printError("ArgumentInvalid", "Crop rectangle must fit inside the input image");
        return 1;
    }

    photonstack::ImageBuffer output;
    output.width = width;
    output.height = height;
    output.channels = input.channels;
    output.format = input.format;
    output.colorEncoding = input.colorEncoding;
    output.sourceBitsPerChannel = input.sourceBitsPerChannel;
    output.pixels.assign(output.sampleCount(), 0.0F);
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto sourceOffset = (static_cast<std::size_t>(y + row) * input.width + x) * input.channels;
        const auto outputOffset = static_cast<std::size_t>(row) * width * output.channels;
        std::copy_n(input.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                    static_cast<std::size_t>(width) * input.channels,
                    output.pixels.begin() + static_cast<std::ptrdiff_t>(outputOffset));
    }

    const auto writeResult = codec.write(output, *outputPath);
    if (!writeResult.ok) {
        printError(writeResult.errorCode, writeResult.message);
        return 1;
    }
    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"crop\",\n"
              << "  \"x\": " << x << ",\n"
              << "  \"y\": " << y << ",\n"
              << "  \"width\": " << width << ",\n"
              << "  \"height\": " << height << ",\n"
              << "  \"output\": \"" << jsonEscape(outputPath->string()) << "\"\n"
              << "}\n";
    return 0;
}

std::vector<std::string> splitWorkflowLine(const std::string& line) {
    std::vector<std::string> args;
    std::string current;
    bool inQuote = false;
    char quote = '\0';

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (!inQuote && c == '#') {
            break;
        }
        if (inQuote) {
            if (c == quote) {
                inQuote = false;
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            inQuote = true;
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        args.push_back(current);
    }
    return args;
}

int runWorkflow(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        printError("InputFileNotFound", "Workflow file could not be opened");
        return 1;
    }

    std::vector<std::vector<std::string>> workflowSteps;
    std::string line;
    while (std::getline(file, line)) {
        auto args = splitWorkflowLine(line);
        if (args.empty()) {
            continue;
        }
        if (args.front() == "photonstack") {
            args.erase(args.begin());
        }
        workflowSteps.push_back(args);
    }

    std::size_t step = 0;
    const auto total = workflowSteps.size();
    for (const auto& args : workflowSteps) {
        ++step;
        const auto command = commandLabel(args);
        printProgressEvent(
            "workflow",
            command,
            "step",
            total == 0 ? 1.0 : static_cast<double>(step - 1) / static_cast<double>(total),
            step,
            total
        );
        const int status = runCommand(args);
        if (status != 0) {
            printError("WorkflowStepFailed", "Workflow failed at step " + std::to_string(step));
            return status;
        }
        printProgressEvent(
            "workflow",
            command,
            "step-complete",
            total == 0 ? 1.0 : static_cast<double>(step) / static_cast<double>(total),
            step,
            total
        );
    }

    std::cout << "{\n"
              << "  \"type\": \"complete\",\n"
              << "  \"command\": \"run\",\n"
              << "  \"steps\": " << step << "\n"
              << "}\n";
    return 0;
}

int runCommandImpl(const std::vector<std::string>& args) {

    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return args.empty() ? 1 : 0;
    }

    if (args[0] == "--version" || args[0] == "version") {
        printVersion();
        return 0;
    }

    if (args[0] == "inspect") {
        if (args.size() != 2) {
            std::cerr << "Usage: photonstack inspect <image>\n";
            return 1;
        }
        return inspectImage(args[1]);
    }

    if (args[0] == "convert") {
        return convertImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "run") {
        if (args.size() != 2) {
            printError("ArgumentInvalid", "Usage: photonstack run <workflow-file>");
            return 1;
        }
        return runWorkflow(args[1]);
    }

    if (args[0] == "fits") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack fits inspect|convert ...");
            return 1;
        }
        if (args[1] == "inspect") {
            if (args.size() != 3) {
                std::cerr << "Usage: photonstack fits inspect <image>\n";
                return 1;
            }
            return inspectImage(args[2]);
        }
        if (args[1] == "convert") {
            return convertImage(std::vector<std::string>(args.begin() + 2, args.end()), "fits convert");
        }
        printError("ArgumentInvalid", "Usage: photonstack fits inspect|convert ...");
        return 1;
    }

    if (args[0] == "raw") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack raw inspect|convert ...");
            return 1;
        }
        if (args[1] == "inspect") {
            if (args.size() != 3) {
                std::cerr << "Usage: photonstack raw inspect <image>\n";
                return 1;
            }
            return inspectImage(args[2]);
        }
        if (args[1] == "convert") {
            return convertImage(std::vector<std::string>(args.begin() + 2, args.end()), "raw convert");
        }
        if (args[1] == "adjust") {
            return adjustRawPreviewImage(std::vector<std::string>(args.begin() + 2, args.end()));
        }
        printError("ArgumentInvalid", "Usage: photonstack raw inspect|convert|adjust ...");
        return 1;
    }

    if (args[0] == "calibrate") {
        return calibrateImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "background") {
        return extractBackground(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "histogram") {
        return printHistogram(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "quality") {
        return analyzeQuality(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "normalize") {
        return normalizeImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "stretch") {
        return stretchImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "curves") {
        return applyCurves(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "local-contrast") {
        return applyLocalContrast(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "denoise") {
        return denoiseImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "sharpen") {
        return sharpenImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "deconvolve") {
        return deconvolveImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "color") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack color neutralize|saturate|remove-green ...");
            return 1;
        }
        if (args[1] == "neutralize" || args[1] == "saturate" || args[1] == "remove-green") {
            return adjustColor(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        printError("ArgumentInvalid", "Usage: photonstack color neutralize|saturate|remove-green ...");
        return 1;
    }

    if (args[0] == "master") {
        return buildMasterFrame(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "mosaic") {
        return buildMosaic(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "register") {
        return registerImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "register-batch") {
        return registerBatch(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "stack") {
        return stackImages(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "drizzle") {
        return drizzleImages(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "artifacts") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack artifacts detect|remove|clean-sequence ...");
            return 1;
        }
        if (args[1] == "clean-sequence") {
            return cleanArtifactSequence(std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[1] == "detect" || args[1] == "remove") {
            return processArtifacts(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        printError("ArgumentInvalid", "Usage: photonstack artifacts detect|remove|clean-sequence ...");
        return 1;
    }

    if (args[0] == "meteors") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack meteors extract|restore|compose ...");
            return 1;
        }
        if (args[1] == "extract" || args[1] == "restore" || args[1] == "compose") {
            return processMeteors(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        printError("ArgumentInvalid", "Usage: photonstack meteors extract|restore|compose ...");
        return 1;
    }

    if (args[0] == "clouds") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack clouds detect|temporal-detect|remove|temporal-remove ...");
            return 1;
        }
        if (args[1] == "detect" || args[1] == "temporal-detect" || args[1] == "remove" ||
            args[1] == "temporal-remove") {
            return processClouds(args[1], std::vector<std::string>(args.begin() + 2, args.end()));
        }
        printError("ArgumentInvalid", "Usage: photonstack clouds detect|temporal-detect|remove|temporal-remove ...");
        return 1;
    }

    if (args[0] == "preview") {
        return previewImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "crop") {
        return cropImage(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    if (args[0] == "stars") {
        if (args.size() < 2) {
            printError("ArgumentInvalid", "Usage: photonstack stars detect|mask|reduce|coma ...");
            return 1;
        }
        if (args[1] == "detect") {
            return detectStars(std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[1] == "mask") {
            return createStarMask(std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[1] == "reduce") {
            return reduceStars(std::vector<std::string>(args.begin() + 2, args.end()));
        }
        if (args[1] == "coma") {
            return reduceComa(std::vector<std::string>(args.begin() + 2, args.end()));
        }
        printError("ArgumentInvalid", "Usage: photonstack stars detect|mask|reduce|coma ...");
        return 1;
    }

    std::cerr << "Unknown command: " << args[0] << "\n\n";
    printUsage();
    return 1;
}

int runCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
        return runCommandImpl(args);
    }

    const auto command = commandLabel(args);
    printProgressEvent(command, command, "start", 0.0);
    int status = 1;
    try {
        status = runCommandImpl(args);
    } catch (const std::bad_alloc&) {
        printError("MemoryAllocationFailed", "The command could not allocate enough memory");
    } catch (const std::exception& error) {
        printError("InternalError", error.what());
    } catch (...) {
        printError("InternalError", "The command failed with an unknown internal error");
    }
    printProgressEvent(command, command, status == 0 ? "complete" : "failed", status == 0 ? 1.0 : 0.0);
    return status;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    return runCommand(args);
}
