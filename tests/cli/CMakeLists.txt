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

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME photonstack_master_registration COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_master_registration.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_channel_composition COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_channels.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_mosaic_seams COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_mosaic_seams.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_background_rbf COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_background_rbf.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_background_review COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_background_review.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_star_layers COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_star_layers.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_wcs_mosaic COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_wcs_mosaic.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_narrowband_sampling COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_narrowband_sampling.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_narrowband_extraction COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_narrowband.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_linear_match COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_linear_match.py" "$<TARGET_FILE:photonstack>")
  add_test(NAME photonstack_psf_measurement COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_psf_measurement.py" "$<TARGET_FILE:photonstack>")
endif()
add_test(NAME photonstack_psf_restoration COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_psf_restoration.py $<TARGET_FILE:photonstack>)

add_test(NAME photonstack_generalized_stretch_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_generalized_stretch.py $<TARGET_FILE:photonstack>)
add_test(NAME photonstack_plate_solve_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_plate_solve.py $<TARGET_FILE:photonstack>)
add_test(NAME photonstack_sky_annotation_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_sky_annotation.py $<TARGET_FILE:photonstack>)
add_test(NAME photonstack_spectral_calibration_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_spectral_calibration.py $<TARGET_FILE:photonstack>)
add_test(NAME photonstack_spectral_profile_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_spectral_profile.py $<TARGET_FILE:photonstack>)
add_test(NAME photonstack_gaia_field_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gaia_field.py $<TARGET_FILE:photonstack>)
add_test(NAME photonstack_narrowband_profile_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_narrowband_profile.py $<TARGET_FILE:photonstack>)
