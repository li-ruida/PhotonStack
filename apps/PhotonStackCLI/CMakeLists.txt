add_executable(photonstack
  main.cpp
  BackgroundReviewCommand.cpp
  StarLayersCommand.cpp
  MasterRegistrationCommand.cpp
  LinearMatchCommand.cpp
  PsfMeasurementCommand.cpp
  PsfRestorationCommand.cpp
  GeneralizedStretchCommand.cpp
  LocalProcess.cpp
  SkyAnnotationCommand.cpp
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
