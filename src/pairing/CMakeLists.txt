nexus_add_module(pairing
  SOURCES
    PairingValidator.cpp
    PairingService.cpp
    ProvisioningController.cpp
  DEPS
    nexus_core
    nexus_logging
    nexus_identity
    nexus_config
    nexus_storage
    nlohmann_json::nlohmann_json)

nexus_add_test(pairing
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/pairing_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/provisioning_controller_test.cpp)
