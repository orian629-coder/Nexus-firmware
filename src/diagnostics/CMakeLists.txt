nexus_add_module(diagnostics
  SOURCES
    DiagnosticService.cpp
  DEPS
    nexus_core
    nexus_logging
    nlohmann_json::nlohmann_json)

nexus_add_test(diagnostics
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/diagnostics_test.cpp)
