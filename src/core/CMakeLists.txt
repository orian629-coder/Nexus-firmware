nexus_add_module(core
  SOURCES
    ErrorCodes.cpp
    ServiceStatus.cpp
    EventBus.cpp
    AtomicFile.cpp
  DEPS
    nlohmann_json::nlohmann_json
    Threads::Threads)

nexus_add_test(core
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/core_test.cpp)
