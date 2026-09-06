nexus_add_module(logging
  SOURCES
    Logger.cpp
    Redaction.cpp
  DEPS
    nexus_core
    spdlog::spdlog)

nexus_add_test(logging
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/logging_test.cpp)
