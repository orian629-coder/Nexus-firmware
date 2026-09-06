# Unit tests are declared per-module via nexus_add_test() in each src/<module>/CMakeLists.txt.
# Here we add cross-module integration and end-to-end tests.

add_executable(nexus_integration_test integration/integration_test.cpp)
target_link_libraries(nexus_integration_test PRIVATE
  nexus_core nexus_config nexus_identity nexus_storage nexus_system
  nexus_network nexus_discovery nexus_pairing nexus_control nexus_status nexus_audio nexus_dsp
  nexus_amplifier nexus_microphone nexus_diagnostics nexus_calibration nexus_web nexus_updater
  GTest::gtest GTest::gtest_main)
set_target_properties(nexus_integration_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
gtest_discover_tests(nexus_integration_test)

add_executable(nexus_e2e_test end_to_end/boot_test.cpp)
target_link_libraries(nexus_e2e_test PRIVATE nexus_app GTest::gtest GTest::gtest_main)
set_target_properties(nexus_e2e_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
gtest_discover_tests(nexus_e2e_test)

# §31 mandatory scenario tests + long-duration soak simulation.
add_executable(nexus_scenario_test end_to_end/scenario_test.cpp end_to_end/soak_test.cpp)
target_link_libraries(nexus_scenario_test PRIVATE
  nexus_core nexus_config nexus_identity nexus_storage nexus_system
  nexus_audio nexus_dsp nexus_amplifier
  GTest::gtest GTest::gtest_main)
set_target_properties(nexus_scenario_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
gtest_discover_tests(nexus_scenario_test)

# Script tests (bash): factory-reset preserves identity.
add_test(NAME script_reset_preserves_identity
         COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_reset.sh)

# Hardware tests are gated off by default (require real audio/thermal devices on the Pi).
if(NEXUS_HW_TESTS)
  add_executable(nexus_hw_test hardware/hardware_test.cpp)
  target_link_libraries(nexus_hw_test PRIVATE nexus_app GTest::gtest GTest::gtest_main)
  gtest_discover_tests(nexus_hw_test)
endif()
