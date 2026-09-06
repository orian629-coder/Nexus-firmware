set(AUDIO_SOURCES
  AudioPacket.cpp
  AudioBuffer.cpp
  StreamSync.cpp
  AudioReceiver.cpp
  PlaybackManager.cpp
  PlaybackEngine.cpp)
set(AUDIO_DEPS nexus_core nexus_logging nexus_config)
if(NOT NEXUS_STUB_HAL)
  list(APPEND AUDIO_SOURCES UdpAudioSource.cpp AlsaAudioOutputHal.cpp)
  list(APPEND AUDIO_DEPS nexus::alsa)
endif()

nexus_add_module(audio
  SOURCES
    ${AUDIO_SOURCES}
  DEPS
    ${AUDIO_DEPS})

nexus_add_test(audio
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/audio_test.cpp)

# Timestamp-honouring playback: needs config (volume/mute) as well as audio, so it gets its own
# target rather than joining the audio module's suite.
if(NEXUS_BUILD_TESTS)
  add_executable(nexus_playback_sync_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/playback_sync_test.cpp)
  target_link_libraries(nexus_playback_sync_test PRIVATE
    nexus_audio nexus_config nexus_core
    GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_playback_sync_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  gtest_discover_tests(nexus_playback_sync_test)
endif()
