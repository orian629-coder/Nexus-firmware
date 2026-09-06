nexus_add_module(status
  SOURCES
    StatusService.cpp
  DEPS
    nexus_core
    nexus_logging
    nexus_identity
    nexus_config
    nlohmann_json::nlohmann_json)

nexus_add_test(status
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/status_test.cpp)
