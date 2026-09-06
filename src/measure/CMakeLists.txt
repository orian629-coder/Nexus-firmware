# Acoustic distance measurement: chirp/impulse stimuli, cross-correlation arrival detection, and
# the latency budget that turns a measured lag into a distance. Header-only — the whole module is
# pure math with no I/O, which is what lets it be validated against recorded files instead of
# requiring a microphone.
add_library(nexus_measure INTERFACE)
target_include_directories(nexus_measure INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(NEXUS_BUILD_TESTS)
  add_executable(nexus_measure_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/measure_test.cpp)
  target_link_libraries(nexus_measure_test PRIVATE
    nexus_measure
    GTest::gtest GTest::gtest_main)
  # The reference WAVs are the AOA prototype's own RTL recording. They are the only way to check
  # this implementation against a known-good answer without a microphone, so the path is compiled
  # in; the test SKIPs (rather than fails) if they are absent on another machine.
  target_compile_definitions(nexus_measure_test PRIVATE
    NEXUS_MEASURE_REF_DIR="$ENV{HOME}/Documents/AOA/measures")
  set_target_properties(nexus_measure_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_measure_test)

  # Speaker-side measurement orchestration: play + record both mics, de-interleave, per-mic distance.
  add_executable(nexus_measurement_service_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/measurement_service_test.cpp)
  target_link_libraries(nexus_measurement_service_test PRIVATE
    nexus_measure
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_measurement_service_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_measurement_service_test)
endif()
