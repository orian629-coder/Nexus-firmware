set(MIC_SOURCES MicrophoneManager.cpp)
set(MIC_DEPS nexus_core nexus_logging)
if(NOT NEXUS_STUB_HAL)
  list(APPEND MIC_SOURCES AlsaMicrophoneHal.cpp)
  list(APPEND MIC_DEPS nexus::alsa)
endif()

nexus_add_module(microphone
  SOURCES
    ${MIC_SOURCES}
  DEPS
    ${MIC_DEPS})

nexus_add_test(microphone
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/microphone_test.cpp)
