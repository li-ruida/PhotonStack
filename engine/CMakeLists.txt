add_library(photonstack_engine STATIC
  src/ArtifactTrailRemover.cpp
  src/BackgroundExtractor.cpp
  src/Calibrator.cpp
  src/CloudRemoval.cpp
  src/ComaReducer.cpp
  src/ColorAdjuster.cpp
  src/Curves.cpp
  src/Deconvolution.cpp
  src/DrizzleStacker.cpp
  src/FitsCodec.cpp
  src/FrameQualityAnalyzer.cpp
  src/FrameNormalizer.cpp
  src/Histogram.cpp
  src/ImageCodec.cpp
  src/ImageInspector.cpp
  src/ImageResizer.cpp
  src/LocalContrast.cpp
  src/RawCodec.cpp
  src/Registration.cpp
  src/ScientificDisplayBridge.cpp
  src/Stacker.cpp
  src/StarDetector.cpp
  src/StarMask.cpp
  src/StarReducer.cpp
  src/Stretch.cpp
  src/MasterFrameBuilder.cpp
  src/MeteorLayerComposer.cpp
  src/MosaicBuilder.cpp
  src/NoiseReducer.cpp
  src/PhotonStackBridge.cpp
  src/Sharpen.cpp
  src/TileProcessor.cpp
)

if(APPLE)
  target_sources(photonstack_engine PRIVATE src/AppleRawDecoder.mm)
  target_compile_options(photonstack_engine PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
endif()

target_include_directories(photonstack_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(photonstack_engine PUBLIC cxx_std_20)

target_compile_definitions(photonstack_engine
  PUBLIC
    PHOTONSTACK_VERSION="${PROJECT_VERSION}"
    PHOTONSTACK_ENGINE_VERSION="${PROJECT_VERSION}"
    PHOTONSTACK_GIT_COMMIT="${PHOTONSTACK_GIT_COMMIT}"
    PHOTONSTACK_BUILD_DATE="${PHOTONSTACK_BUILD_DATE}"
)

if(APPLE)
  target_link_libraries(photonstack_engine
    PUBLIC
      "-framework CoreFoundation"
      "-framework CoreGraphics"
      "-framework CoreImage"
      "-framework Foundation"
      "-framework ImageIO"
      "-framework Metal"
  )
endif()

if(MSVC)
  target_compile_options(photonstack_engine PRIVATE /W4)
else()
  target_compile_options(photonstack_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
