add_executable(photonstack_engine_tests
  ImageInspectorTests.cpp
)

target_link_libraries(photonstack_engine_tests
  PRIVATE
    photonstack_engine
)

target_compile_features(photonstack_engine_tests PRIVATE cxx_std_20)

add_test(NAME photonstack_engine_tests COMMAND photonstack_engine_tests)

