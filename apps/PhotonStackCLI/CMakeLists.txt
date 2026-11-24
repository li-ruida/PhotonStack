add_executable(photonstack
  main.cpp
  SerCommand.cpp
  XisfCommand.cpp
  ImagePluginCommand.cpp
  TimeSeriesCommand.cpp
  BackgroundReviewCommand.cpp
  StarLayersCommand.cpp
  LinearStarLayersCommand.cpp
  MasterRegistrationCommand.cpp
  LinearMatchCommand.cpp
  PsfMeasurementCommand.cpp
  PsfRestorationCommand.cpp
  GeneralizedStretchCommand.cpp
  LocalProcess.cpp
  SkyAnnotationCommand.cpp
  SpectralCalibrationCommand.cpp
  SpectralProfileCommand.cpp
  SpectralCatalogCommand.cpp
  CalibrationMetadataCommand.cpp
  AcquisitionDeclarationCommand.cpp
  SensorQualityCommand.cpp
  SensorMasterCommand.cpp
  CalibrationJobCommand.cpp
  PlateSolveCommand.cpp
  ChannelCommand.cpp
  NarrowbandCommand.cpp
  FrequencyStackCommand.cpp
  DeepSkyCommand.cpp
  DeepSkyPreflight.cpp
)

target_link_libraries(photonstack
  PRIVATE
    photonstack_engine
)

target_compile_features(photonstack PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(photonstack PRIVATE /W4)
else()
  target_compile_options(photonstack PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(photonstack PRIVATE PHOTONSTACK_CATALOG_DIR="${PROJECT_SOURCE_DIR}/resources/OpenNGC")

# XML and deflate are system dependencies; optional codecs are linked statically
# so packaged macOS applications do not depend on a Homebrew installation.
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(photonstack PRIVATE LibXml2::LibXml2 ZLIB::ZLIB)
foreach(codec IN ITEMS lz4 zstd)
  string(TOUPPER "${codec}" upper)
  find_path(PHOTONSTACK_${upper}_INCLUDE ${codec}.h PATHS /opt/homebrew/include /usr/local/include)
  find_file(PHOTONSTACK_${upper}_ARCHIVE lib${codec}.a PATHS /opt/homebrew/lib /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu)
  if(PHOTONSTACK_${upper}_INCLUDE AND PHOTONSTACK_${upper}_ARCHIVE)
    target_include_directories(photonstack PRIVATE ${PHOTONSTACK_${upper}_INCLUDE})
    target_link_libraries(photonstack PRIVATE ${PHOTONSTACK_${upper}_ARCHIVE})
    target_compile_definitions(photonstack PRIVATE PHOTONSTACK_XISF_${upper}=1)
  else()
    message(STATUS "XISF ${codec} decoding disabled (static library not found)")
  endif()
endforeach()
