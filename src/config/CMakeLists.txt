nexus_add_module(config
  SOURCES
    DefaultConfig.cpp
    ConfigValidator.cpp
    ConfigManager.cpp
  DEPS
    nexus_core
    nexus_logging
    nlohmann_json::nlohmann_json)

nexus_add_test(config
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/config_test.cpp)
