nexus_add_module(system
  SOURCES
    StateMachine.cpp
    SystemManager.cpp
    Watchdog.cpp
  DEPS
    nexus_core
    nexus_logging)

nexus_add_test(system
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/system_test.cpp)
