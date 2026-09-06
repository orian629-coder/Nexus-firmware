nexus_add_module(dsp
  SOURCES
    Equalizer.cpp
    DspEngine.cpp
  DEPS
    nexus_core
    nexus_logging)

nexus_add_test(dsp
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/dsp_test.cpp)
