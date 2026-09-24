add_executable(photonstack
  main.cpp
  FrequencyStackCommand.cpp
  DeepSkyCommand.cpp
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
