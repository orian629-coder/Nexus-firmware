nexus_add_module(identity
  SOURCES
    Crypto.cpp
    KeyManager.cpp
    DeviceIdentity.cpp
    ApCredentials.cpp
  DEPS
    nexus_core
    nexus_logging
    nexus_storage
    nexus::sodium
    nlohmann_json::nlohmann_json)

nexus_add_test(identity
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/identity_test.cpp)

# Crypto helpers live in the identity lib; give them their own test executable.
if(NEXUS_BUILD_TESTS)
  add_executable(nexus_crypto_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/crypto_test.cpp)
  target_link_libraries(nexus_crypto_test PRIVATE nexus_identity GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_crypto_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  gtest_discover_tests(nexus_crypto_test)
endif()

if(NEXUS_BUILD_TESTS)
  add_executable(nexus_ap_credentials_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/ap_credentials_test.cpp)
  target_link_libraries(nexus_ap_credentials_test PRIVATE nexus_identity GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_ap_credentials_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  gtest_discover_tests(nexus_ap_credentials_test)
endif()
