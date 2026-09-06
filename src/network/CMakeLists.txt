set(NETWORK_SOURCES NetworkManager.cpp)
if(NOT NEXUS_STUB_HAL)
  list(APPEND NETWORK_SOURCES NmcliNetworkHal.cpp)
endif()

nexus_add_module(network
  SOURCES
    ${NETWORK_SOURCES}
  DEPS
    nexus_core
    nexus_logging)

nexus_add_test(network
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/network_test.cpp)
