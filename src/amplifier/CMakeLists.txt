set(AMP_SOURCES AmplifierManager.cpp)
set(AMP_DEPS nexus_core nexus_logging)
if(NOT NEXUS_STUB_HAL)
  list(APPEND AMP_SOURCES GpioAmplifierHal.cpp)
  list(APPEND AMP_DEPS nexus::gpiod)
endif()

nexus_add_module(amplifier
  SOURCES
    ${AMP_SOURCES}
  DEPS
    ${AMP_DEPS})

nexus_add_test(amplifier
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/amplifier_test.cpp)
