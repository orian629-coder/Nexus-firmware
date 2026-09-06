set(WEB_SOURCES
  ApiRouter.cpp
  QrEncoder.cpp
  WebServer.cpp)
set(WEB_DEPS nexus_core nexus_logging nlohmann_json::nlohmann_json)
if(NOT NEXUS_STUB_HAL)
  list(APPEND WEB_SOURCES HttplibWebTransport.cpp)
  list(APPEND WEB_DEPS httplib)
endif()

nexus_add_module(web
  SOURCES
    ${WEB_SOURCES}
  DEPS
    ${WEB_DEPS})

nexus_add_test(web
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/web_test.cpp)

# QR encoder test links the web module (where QrEncoder lives).
if(NEXUS_BUILD_TESTS)
  add_executable(nexus_qr_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/qr_test.cpp)
  target_link_libraries(nexus_qr_test PRIVATE nexus_web GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_qr_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_qr_test)
endif()
