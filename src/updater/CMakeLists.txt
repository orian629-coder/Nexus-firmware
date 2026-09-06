nexus_add_module(updater
  SOURCES
    SignatureValidator.cpp
    RollbackManager.cpp
    Installer.cpp
    UpdateManager.cpp
  DEPS
    nexus_core
    nexus_logging
    nexus_identity)

nexus_add_test(updater
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/updater_test.cpp)
