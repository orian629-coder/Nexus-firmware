set(DISCOVERY_SOURCES DiscoveryService.cpp)
set(DISCOVERY_DEPS nexus_core nexus_logging)
if(NOT NEXUS_STUB_HAL)
  list(APPEND DISCOVERY_SOURCES AvahiDiscoveryHal.cpp)
  list(APPEND DISCOVERY_DEPS nexus::avahi)
endif()

nexus_add_module(discovery
  SOURCES
    ${DISCOVERY_SOURCES}
  DEPS
    ${DISCOVERY_DEPS})

nexus_add_test(discovery
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/discovery_test.cpp)
