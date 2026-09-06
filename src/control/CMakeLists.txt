set(CONTROL_SOURCES
  Command.cpp
  CommandValidator.cpp
  CommandHistory.cpp
  CommandExecutor.cpp
  CommandRouter.cpp
  CommandServer.cpp)
if(NOT NEXUS_STUB_HAL)
  list(APPEND CONTROL_SOURCES TcpCommandTransport.cpp)
endif()

nexus_add_module(control
  SOURCES
    ${CONTROL_SOURCES}
  DEPS
    nexus_core
    nexus_logging
    nexus_identity
    nexus_config
    nlohmann_json::nlohmann_json)

nexus_add_test(control
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/control_test.cpp)

# Per-speaker trim and polarity span control (the commands) and dsp (the effect), so this test
# links both rather than living in either module's own suite.
if(NEXUS_BUILD_TESTS)
  add_executable(nexus_gain_phase_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/gain_phase_test.cpp)
  target_link_libraries(nexus_gain_phase_test PRIVATE
    nexus_control nexus_config nexus_dsp nexus_core
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_gain_phase_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_gain_phase_test)
endif()
