set(BLUETOOTH_SOURCES BluetoothManager.cpp)
if(NOT NEXUS_STUB_HAL)
  list(APPEND BLUETOOTH_SOURCES BluetoothctlHal.cpp)
endif()

nexus_add_module(bluetooth
  SOURCES
    ${BLUETOOTH_SOURCES}
  DEPS
    nexus_core
    nexus_logging)

nexus_add_test(bluetooth
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/bluetooth_test.cpp)
