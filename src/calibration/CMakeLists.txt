nexus_add_module(calibration
  SOURCES
    RoomMeasurement.cpp
    CalibrationProfiles.cpp
    CalibrationManager.cpp
  DEPS
    nexus_core
    nexus_logging
    nexus_config
    nexus_dsp
    nexus_microphone
    nlohmann_json::nlohmann_json)

nexus_add_test(calibration
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/calibration_test.cpp)
