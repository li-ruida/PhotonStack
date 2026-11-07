add_test(
  NAME photonstack_cli_invalid_numeric_arguments
  COMMAND
    ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:photonstack>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/InvalidNumericArguments.cmake"
)

add_executable(photonstack_frequency_cli_tests
  FrequencyStackTests.cpp
  ../../apps/PhotonStackCLI/FrequencyStackCommand.cpp
)
target_include_directories(photonstack_frequency_cli_tests PRIVATE ../../apps/PhotonStackCLI)
target_link_libraries(photonstack_frequency_cli_tests PRIVATE photonstack_engine)
target_compile_features(photonstack_frequency_cli_tests PRIVATE cxx_std_20)
add_test(NAME photonstack_frequency_cli_tests COMMAND photonstack_frequency_cli_tests)

add_test(NAME photonstack_nonlocal_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/NonlocalDenoise.cmake")

add_test(NAME photonstack_continuum_cooling_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/ContinuumCooling.cmake")
add_test(NAME photonstack_display_grid_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/DisplayGridReduction.cmake")

add_test(NAME photonstack_review_preview_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/ReviewPreview.cmake")

add_test(NAME photonstack_develop_reference_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/DevelopReference.cmake")

add_test(NAME photonstack_command_help_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/CommandHelp.cmake")

add_executable(photonstack_deep_sky_preflight_tests
  DeepSkyPreflightTests.cpp ../../apps/PhotonStackCLI/DeepSkyPreflight.cpp)
target_include_directories(photonstack_deep_sky_preflight_tests PRIVATE ../../apps/PhotonStackCLI)
target_link_libraries(photonstack_deep_sky_preflight_tests PRIVATE photonstack_engine)
target_compile_features(photonstack_deep_sky_preflight_tests PRIVATE cxx_std_20)
add_test(NAME photonstack_deep_sky_preflight_tests COMMAND photonstack_deep_sky_preflight_tests)
add_test(NAME photonstack_deep_sky_preflight_cli_tests COMMAND ${CMAKE_COMMAND}
  "-DCLI=$<TARGET_FILE:photonstack>" -P "${CMAKE_CURRENT_SOURCE_DIR}/DeepSkyPreflight.cmake")
