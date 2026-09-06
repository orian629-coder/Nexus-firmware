# The Application library links every module (real + stub), so main and integration tests can
# bring up the whole system.
add_library(nexus_app STATIC Application.cpp)
target_include_directories(nexus_app PUBLIC ${NEXUS_SRC_DIR})
target_link_libraries(nexus_app PUBLIC
  nexus_core
  nexus_logging
  nexus_config
  nexus_storage
  nexus_identity
  nexus_system
  nexus_network
  nexus_discovery
  nexus_pairing
  nexus_bluetooth
  nexus_control
  nexus_audio
  nexus_dsp
  nexus_amplifier
  nexus_microphone
  nexus_calibration
  nexus_measure
  nexus_status
  nexus_diagnostics
  nexus_updater
  nexus_web)
nexus_apply_warnings(nexus_app)
set_target_properties(nexus_app PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_executable(nexus-speaker main.cpp)
target_link_libraries(nexus-speaker PRIVATE nexus_app)
set_target_properties(nexus-speaker PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
