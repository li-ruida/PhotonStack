add_test(
  NAME photonstack_cli_invalid_numeric_arguments
  COMMAND
    ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:photonstack>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/InvalidNumericArguments.cmake"
)
