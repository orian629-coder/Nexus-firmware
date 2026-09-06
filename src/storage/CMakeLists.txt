nexus_add_module(storage
  SOURCES
    SecureStorage.cpp
  DEPS
    nexus_core)

nexus_add_test(storage
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/storage_test.cpp)
