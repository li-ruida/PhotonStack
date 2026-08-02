#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PhotonStackMosaicProjection {
    PHOTONSTACK_MOSAIC_PROJECTION_PLANAR = 0,
    PHOTONSTACK_MOSAIC_PROJECTION_CYLINDRICAL = 1,
} PhotonStackMosaicProjection;

typedef enum PhotonStackMosaicLayout {
    PHOTONSTACK_MOSAIC_LAYOUT_HORIZONTAL = 0,
    PHOTONSTACK_MOSAIC_LAYOUT_GRID = 1,
} PhotonStackMosaicLayout;

typedef enum PhotonStackMosaicAlignment {
    PHOTONSTACK_MOSAIC_ALIGNMENT_MANUAL = 0,
    PHOTONSTACK_MOSAIC_ALIGNMENT_AUTO = 1,
} PhotonStackMosaicAlignment;

typedef enum PhotonStackMosaicBlendMode {
    PHOTONSTACK_MOSAIC_BLEND_AVERAGE = 0,
    PHOTONSTACK_MOSAIC_BLEND_FEATHER = 1,
    PHOTONSTACK_MOSAIC_BLEND_MULTIBAND = 2,
} PhotonStackMosaicBlendMode;

typedef enum PhotonStackImageReadBackend {
    PHOTONSTACK_IMAGE_READ_BACKEND_UNKNOWN = 0,
    PHOTONSTACK_IMAGE_READ_BACKEND_IMAGEIO = 1,
    PHOTONSTACK_IMAGE_READ_BACKEND_APPLE_RAW = 2,
    PHOTONSTACK_IMAGE_READ_BACKEND_FITS = 3,
} PhotonStackImageReadBackend;

typedef enum PhotonStackRawWhiteBalanceMode {
    PHOTONSTACK_RAW_WHITE_BALANCE_CAMERA = 0,
    PHOTONSTACK_RAW_WHITE_BALANCE_AUTO = 1,
    PHOTONSTACK_RAW_WHITE_BALANCE_DAYLIGHT = 2,
    PHOTONSTACK_RAW_WHITE_BALANCE_MANUAL = 3,
} PhotonStackRawWhiteBalanceMode;

typedef enum PhotonStackRawBlackLevelMode {
    PHOTONSTACK_RAW_BLACK_LEVEL_CAMERA = 0,
    PHOTONSTACK_RAW_BLACK_LEVEL_AUTO = 1,
    PHOTONSTACK_RAW_BLACK_LEVEL_MANUAL = 2,
} PhotonStackRawBlackLevelMode;

typedef enum PhotonStackRawDemosaicQuality {
    PHOTONSTACK_RAW_DEMOSAIC_FAST = 0,
    PHOTONSTACK_RAW_DEMOSAIC_BALANCED = 1,
    PHOTONSTACK_RAW_DEMOSAIC_HIGH = 2,
} PhotonStackRawDemosaicQuality;

typedef enum PhotonStackCurveChannel {
    PHOTONSTACK_CURVE_CHANNEL_RGB = 0,
    PHOTONSTACK_CURVE_CHANNEL_RED = 1,
    PHOTONSTACK_CURVE_CHANNEL_GREEN = 2,
    PHOTONSTACK_CURVE_CHANNEL_BLUE = 3,
    PHOTONSTACK_CURVE_CHANNEL_LUMINANCE = 4,
} PhotonStackCurveChannel;

typedef struct PhotonStackCurvePoint {
    float input;
    float output;
} PhotonStackCurvePoint;

typedef struct PhotonStackCurvePreviewRequest {
    uint32_t width;
    PhotonStackRawWhiteBalanceMode rawWhiteBalanceMode;
    float rawManualWhiteBalanceTemperature;
    float rawManualWhiteBalanceTint;
    float rawExposureBias;
    PhotonStackRawBlackLevelMode rawBlackLevelMode;
    float rawManualBlackLevel;
    PhotonStackRawDemosaicQuality rawDemosaicQuality;
    uint8_t rawLinearOutput;
    uint8_t retainDecodedSource;
    PhotonStackCurveChannel curveChannel;
    const PhotonStackCurvePoint* curvePoints;
    size_t curvePointCount;
} PhotonStackCurvePreviewRequest;

typedef struct PhotonStackCurvePreviewRunResult {
    uint8_t ok;
    uint32_t width;
    uint32_t height;
    uint32_t decodedWidth;
    uint32_t decodedHeight;
    PhotonStackImageReadBackend backend;
    uint8_t usedFallback;
} PhotonStackCurvePreviewRunResult;

typedef struct PhotonStackMosaicRequest {
    uint32_t overlapPixels;
    PhotonStackMosaicProjection projection;
    PhotonStackMosaicLayout layout;
    PhotonStackMosaicAlignment alignment;
    PhotonStackMosaicBlendMode blendMode;
    uint8_t exposureMatching;
    uint32_t columns;
    uint32_t previewWidth;
} PhotonStackMosaicRequest;

typedef struct PhotonStackMosaicRunResult {
    uint8_t ok;
    uint32_t width;
    uint32_t height;
    uint8_t autoAligned;
    uint32_t matches;
    uint32_t fallbackPanels;
} PhotonStackMosaicRunResult;

const char* photonstack_engine_version(void);
PhotonStackMosaicRequest photonstack_default_mosaic_request(void);
PhotonStackCurvePreviewRequest photonstack_default_curve_preview_request(void);
void photonstack_clear_curve_preview_cache(void);
PhotonStackMosaicRunResult photonstack_mosaic_paths(
    const char* const* inputs,
    size_t inputCount,
    const char* output,
    PhotonStackMosaicRequest request,
    char* errorBuffer,
    size_t errorBufferSize
);
PhotonStackCurvePreviewRunResult photonstack_curve_preview_path(
    const char* input,
    const char* output,
    PhotonStackCurvePreviewRequest request,
    char* fallbackErrorCodeBuffer,
    size_t fallbackErrorCodeBufferSize,
    char* fallbackErrorMessageBuffer,
    size_t fallbackErrorMessageBufferSize,
    char* errorBuffer,
    size_t errorBufferSize
);

#ifdef __cplusplus
}
#endif
