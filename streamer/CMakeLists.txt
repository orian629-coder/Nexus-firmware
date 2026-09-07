# Nexus Streamer — the audio sender + controller counterpart to the speaker sink.
#
# The streamer links the speaker's existing per-module static libraries directly (they are already
# isolated via NexusModule.cmake), so the wire contract (packet encoder, Command struct + canonical
# string, crypto) is SHARED by construction rather than re-implemented. Streamer-local code lives
# under streamer/src and includes both the streamer root and the speaker src root.

set(NEXUS_STREAMER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ── streamer control: signed commands (CommandSigner) + command client. The real TCP transport is
# compiled only when NEXUS_STREAMER_REAL_NET is on; the client logic itself is transport-agnostic
# (ILineTransport) so it is unit-testable off-target. ──
set(STREAMER_CONTROL_SOURCES src/control/CommandSigner.cpp src/control/CommandClient.cpp)
if(NEXUS_STREAMER_REAL_NET)
  list(APPEND STREAMER_CONTROL_SOURCES src/control/TcpLineTransport.cpp)
endif()

add_library(nexus_streamer_control STATIC ${STREAMER_CONTROL_SOURCES})
target_include_directories(nexus_streamer_control PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_control PUBLIC
  nexus_core
  nexus_control      # control::Command + canonicalString()
  nexus_identity     # crypto::signEd25519Base64
  nlohmann_json::nlohmann_json)
if(NEXUS_STREAMER_REAL_NET)
  target_compile_definitions(nexus_streamer_control PUBLIC NEXUS_STREAMER_REAL_NET=1)
endif()
nexus_apply_warnings(nexus_streamer_control)
set_target_properties(nexus_streamer_control PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer discovery: advertise _nexus-streamer._tcp + browse _nexus-speaker._tcp. The real Avahi
# implementation needs the avahi client lib (Pi build); the stub + interface are header-only so the
# onboarding flow is testable off-target. ──
if(NOT NEXUS_STUB_HAL AND TARGET nexus::avahi)
  add_library(nexus_streamer_discovery STATIC src/discovery/AvahiStreamerDiscovery.cpp)
  target_include_directories(nexus_streamer_discovery PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_discovery PUBLIC nexus_core nexus_logging nexus::avahi)
  target_compile_definitions(nexus_streamer_discovery PUBLIC NEXUS_STREAMER_AVAHI=1)
  nexus_apply_warnings(nexus_streamer_discovery)
  set_target_properties(nexus_streamer_discovery PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
else()
  add_library(nexus_streamer_discovery INTERFACE)
  target_include_directories(nexus_streamer_discovery INTERFACE ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_discovery INTERFACE nexus_core)
endif()

# ── streamer network scanner (installer "Scan Network"): probe the subnet over HTTP for nexus
# speakers. Needs the httplib client, so real-net only; a stub-only build gets an empty interface. ──
if(NEXUS_STREAMER_REAL_NET)
  add_library(nexus_streamer_scan STATIC src/discovery/NetworkScanner.cpp)
  target_include_directories(nexus_streamer_scan PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_scan PUBLIC nexus_core httplib nlohmann_json::nlohmann_json)
  target_compile_definitions(nexus_streamer_scan PUBLIC NEXUS_STREAMER_REAL_NET=1)
  nexus_apply_warnings(nexus_streamer_scan)
  set_target_properties(nexus_streamer_scan PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
else()
  add_library(nexus_streamer_scan INTERFACE)
  target_include_directories(nexus_streamer_scan INTERFACE ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_scan INTERFACE nexus_core)
endif()

# ── Wi-Fi onboarding: find speakers that are not on any network yet (they raise a "Nexus-Setup" AP)
# and temporarily associate with one so the pairing handshake can reach it. Both shell out to nmcli
# and pull in no network client library, so unlike nexus_streamer_scan they build in every mode. ──
add_library(nexus_streamer_wifi STATIC src/discovery/ApScanner.cpp src/discovery/ApJoiner.cpp)
target_include_directories(nexus_streamer_wifi PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_wifi PUBLIC nexus_core)
nexus_apply_warnings(nexus_streamer_wifi)
set_target_properties(nexus_streamer_wifi PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer identity: persistent Ed25519 signing key on disk (0600) ──
add_library(nexus_streamer_identity STATIC src/identity/StreamerIdentity.cpp)
target_include_directories(nexus_streamer_identity PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_identity PUBLIC nexus_core nexus_identity)
nexus_apply_warnings(nexus_streamer_identity)
set_target_properties(nexus_streamer_identity PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer pairing: seal wifi + sign PairingRequest, using shared crypto and PairingTypes ──
add_library(nexus_streamer_pairing STATIC src/pairing/PairingClient.cpp)
target_include_directories(nexus_streamer_pairing PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_pairing PUBLIC
  nexus_core
  nexus_identity     # crypto::sealTo / signEd25519Base64
  nexus_pairing      # pairing::PairingRequest::canonicalString()
  nlohmann_json::nlohmann_json)
nexus_apply_warnings(nexus_streamer_pairing)
set_target_properties(nexus_streamer_pairing PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer send pipeline: source → timestamper → packetizer (reuses packPacket) → sink ──
# UdpPacketSink needs real sockets, so it is compiled only when NEXUS_STUB_HAL is off (matching the
# speaker's real/stub HAL split). The header-only pipeline (Timestamper/Packetizer/StreamSender/
# MemoryPacketSink/sources) is always available so the whole path is testable off-target.
if(NEXUS_STREAMER_REAL_NET)
  # Real UDP sink compiled in — plain BSD sockets, so this works on a dev host (macOS/Linux) as well
  # as the Pi. This is what lets the Mac act as a real streamer sending to a speaker.
  add_library(nexus_streamer_send STATIC src/send/UdpPacketSink.cpp)
  target_include_directories(nexus_streamer_send PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_send PUBLIC
    nexus_core
    nexus_audio)   # audio::AudioPacket + packPacket (shared wire encoder)
  target_compile_definitions(nexus_streamer_send PUBLIC NEXUS_STREAMER_REAL_NET=1)
  nexus_apply_warnings(nexus_streamer_send)
  set_target_properties(nexus_streamer_send PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
else()
  # No-sockets build: pipeline is header-only (no UdpPacketSink); an INTERFACE library carries the
  # include dirs and the shared audio dependency.
  add_library(nexus_streamer_send INTERFACE)
  target_include_directories(nexus_streamer_send INTERFACE ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_send INTERFACE nexus_core nexus_audio)
endif()

# ── streamer sources: where audio comes FROM (file, stdin, process, capture device) ──
#
# Its own library because both the app (which constructs sources) and the web router (which lists
# and validates them) need it. Before this existed, WavFileSource.cpp was compiled into the app
# only, so any use of the source layer from the web side linked by luck.
add_library(nexus_streamer_sources STATIC
  src/sources/WavFileSource.cpp
  src/sources/CaptureDevices.cpp)
target_include_directories(nexus_streamer_sources PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_sources PUBLIC nexus_core nlohmann_json::nlohmann_json)
nexus_apply_warnings(nexus_streamer_sources)
set_target_properties(nexus_streamer_sources PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer web: control UI + API router. The router is pure (testable with StubWebTransport from
# nexus_web); the httplib transport is compiled only with real sockets. ──
set(STREAMER_WEB_SOURCES src/web/StreamerApiRouter.cpp)
if(NEXUS_STREAMER_REAL_NET)
  list(APPEND STREAMER_WEB_SOURCES src/web/HttplibStreamerTransport.cpp)
endif()

add_library(nexus_streamer_web STATIC ${STREAMER_WEB_SOURCES})
target_include_directories(nexus_streamer_web PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_web PUBLIC
  nexus_core
  nexus_web                # IWebTransport / HttpRequest / HttpResponse / StubWebTransport + QrEncoder
  nexus_control            # cmd:: name constants
  nexus_streamer_control   # CommandReply type
  nexus_streamer_pairing   # PairingParams / PairingReply
  nexus_streamer_discovery # DiscoveredSpeaker
  nexus_streamer_sources   # SourceSpec + capture device enumeration for GET /api/sources
  nlohmann_json::nlohmann_json)
if(NEXUS_STREAMER_REAL_NET)
  target_link_libraries(nexus_streamer_web PUBLIC httplib)
  target_compile_definitions(nexus_streamer_web PUBLIC NEXUS_STREAMER_REAL_NET=1)
endif()
nexus_apply_warnings(nexus_streamer_web)
set_target_properties(nexus_streamer_web PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer audio engine: the paced send loop as a long-running service ──
#
# MODULE BOUNDARY (enforced by this link list, not by convention): nexus_streamer_audio links ONLY
# the send pipeline and core. It must NEVER link nexus_streamer_web or nexus_streamer_control — the
# audio module does not depend on the interface, and does not know how commands or Wi-Fi work. A
# violation of that rule shows up as a link error rather than as a review comment.
add_library(nexus_streamer_audio STATIC src/audio/AudioEngine.cpp)
target_include_directories(nexus_streamer_audio PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_audio PUBLIC
  nexus_core           # IService + EventBus
  nexus_streamer_send) # Timestamper/Packetizer/IPacketSink (+ shared packPacket via nexus_audio)
nexus_apply_warnings(nexus_streamer_audio)
set_target_properties(nexus_streamer_audio PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer app: composition root (services + ordered startup/shutdown) ──
# This is the layer that unifies "serve" and "stream": the web UI, control channel, and audio engine
# are services in one process instead of mutually exclusive main() branches.
add_library(nexus_streamer_app STATIC
  src/app/StreamerApp.cpp
  src/app/CommandGateway.cpp
  src/config/StreamerConfigManager.cpp
  src/net/WifiSignal.cpp)
target_include_directories(nexus_streamer_app PUBLIC ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_app PUBLIC
  nexus_core
  nexus_streamer_audio
  nexus_streamer_control
  nexus_streamer_web
  nexus_streamer_pairing
  nexus_streamer_identity
  nexus_streamer_discovery
  nexus_streamer_scan  # StreamerApp::startup calls scanSubnet/localSubnetOr (NetworkScanner)
  nexus_streamer_wifi  # setup-AP scan + temporary AP join for onboarding a fresh speaker
  nexus_streamer_sources
  nexus_dsp            # the SPEAKER's DSP library, reused for the master chain (see MasterDsp.h)
  nexus_identity)
nexus_apply_warnings(nexus_streamer_app)
set_target_properties(nexus_streamer_app PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── streamer executable ──
add_executable(nexus-streamer src/main/StreamerMain.cpp)
target_include_directories(nexus-streamer PRIVATE ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus-streamer PRIVATE
  nexus_streamer_app
  nexus_streamer_scan)
set_target_properties(nexus-streamer PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# ── tests: sign here, verify with the speaker's real CommandValidator ──
if(NEXUS_BUILD_TESTS)
  add_executable(nexus_streamer_control_test tests/unit/command_signer_test.cpp)
  target_include_directories(nexus_streamer_control_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_control_test PRIVATE
    nexus_streamer_control
    nexus_control       # CommandValidator (the real speaker-side verifier)
    nexus_identity
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_control_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_control_test)

  # Capture-device source layer. Pure parts (escaping, classification, spec validation) must pass on
  # any host, including one with no audio backend at all.
  add_executable(nexus_streamer_capture_test tests/unit/capture_devices_test.cpp)
  target_include_directories(nexus_streamer_capture_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_capture_test PRIVATE
    nexus_streamer_sources
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_capture_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_capture_test)

  # Speaker telemetry: playback health mirrored from GET_STATUS into the state store.
  add_executable(nexus_streamer_telemetry_test tests/unit/telemetry_test.cpp)
  target_include_directories(nexus_streamer_telemetry_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_telemetry_test PRIVATE
    nexus_core
    nlohmann_json::nlohmann_json
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_telemetry_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_telemetry_test)

  # Signal metering: the input/output VU readings shown in the UI.
  add_executable(nexus_streamer_meter_test tests/unit/level_meter_test.cpp)
  target_include_directories(nexus_streamer_meter_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_meter_test PRIVATE GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_meter_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_meter_test)

  # Master DSP chain: bypass safety, EQ effect, clamping, and config round-trip.
  add_executable(nexus_streamer_dsp_test tests/unit/master_dsp_test.cpp)
  target_include_directories(nexus_streamer_dsp_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_dsp_test PRIVATE
    nexus_dsp
    nlohmann_json::nlohmann_json
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_dsp_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_dsp_test)

  # E2E: the streamer's real CommandClient drives the real speaker CommandServer in-process.
  add_executable(nexus_streamer_command_client_test tests/unit/command_client_test.cpp)
  target_include_directories(nexus_streamer_command_client_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_command_client_test PRIVATE
    nexus_streamer_control
    nexus_control       # real CommandServer + StubCommandTransport
    nexus_config
    nexus_identity
    nexus_storage
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_command_client_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_command_client_test)

  # Web control API router: UI action → router → real CommandClient → real speaker CommandServer.
  add_executable(nexus_streamer_web_test tests/unit/web_router_test.cpp)
  target_include_directories(nexus_streamer_web_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_web_test PRIVATE
    nexus_streamer_web
    nexus_streamer_control
    nexus_streamer_pairing
    nexus_streamer_discovery
    nexus_control
    nexus_pairing
    nexus_config
    nexus_identity
    nexus_storage
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_web_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_web_test)

  # Persistent streamer identity (key gen/reload, 0600, sign/verify consistency).
  add_executable(nexus_streamer_identity_test tests/unit/identity_test.cpp)
  target_include_directories(nexus_streamer_identity_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_identity_test PRIVATE
    nexus_streamer_identity nexus_identity GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_identity_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_identity_test)

  # Contract guard: the streamer must derive AP credentials from the same shared module as the
  # speaker (Task 1's nexus::identity::deriveApCredentials), not a re-implementation.
  add_executable(nexus_streamer_ap_credentials_test tests/unit/ap_credentials_contract_test.cpp)
  target_include_directories(nexus_streamer_ap_credentials_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_ap_credentials_test PRIVATE
    nexus_identity GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_ap_credentials_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_ap_credentials_test)

  # Discovery interface/stub contract (real Avahi path exercised on the Pi).
  add_executable(nexus_streamer_discovery_test tests/unit/discovery_test.cpp)
  target_include_directories(nexus_streamer_discovery_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_discovery_test PRIVATE
    nexus_streamer_discovery nexus_core GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_discovery_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_discovery_test)

  # E2E: the streamer's PairingClient seals+signs a request the real PairingService accepts.
  add_executable(nexus_streamer_pairing_client_test tests/unit/pairing_client_test.cpp)
  target_include_directories(nexus_streamer_pairing_client_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_pairing_client_test PRIVATE
    nexus_streamer_pairing
    nexus_streamer_control  # ILineTransport
    nexus_pairing           # real PairingService
    nexus_config
    nexus_identity
    nexus_storage
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_pairing_client_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_pairing_client_test)

  # Phase-1 proof: one process serves the control API AND sends audio. Drives the real StreamerApp
  # (audio engine + gateway + router) against a real in-process speaker CommandServer.
  add_executable(nexus_streamer_app_test tests/unit/app_lifecycle_test.cpp)
  target_include_directories(nexus_streamer_app_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_app_test PRIVATE
    nexus_streamer_app
    nexus_control       # real CommandServer + StubCommandTransport
    nexus_audio         # unpackPacket
    nexus_config
    nexus_identity
    nexus_storage
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_app_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_app_test)

  # Persistence, zones, and real audio sources.
  add_executable(nexus_streamer_persist_test tests/unit/persistence_zones_test.cpp)
  target_include_directories(nexus_streamer_persist_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_persist_test PRIVATE
    nexus_streamer_app
    nexus_core
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_persist_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_persist_test)

  # Phase-2 proof: the speaker is the source of truth. Drives the real MonitorService + state store
  # against a real in-process speaker CommandServer.
  add_executable(nexus_streamer_state_test tests/unit/state_monitor_test.cpp)
  target_include_directories(nexus_streamer_state_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_state_test PRIVATE
    nexus_streamer_app
    nexus_control
    nexus_config
    nexus_identity
    nexus_storage
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_state_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_state_test)

  # Send pipeline: packetize here, decode with the speaker's real unpackPacket and assert the exact
  # PCM round-trip, future timestamps, and monotonic sequence — the Phase-1 wire-contract proof.
  add_executable(nexus_streamer_send_test tests/unit/send_pipeline_test.cpp)
  target_include_directories(nexus_streamer_send_test PRIVATE
    ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
  target_link_libraries(nexus_streamer_send_test PRIVATE
    nexus_streamer_send
    nexus_audio         # unpackPacket (the real speaker-side decoder)
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_streamer_send_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_streamer_send_test)
endif()
