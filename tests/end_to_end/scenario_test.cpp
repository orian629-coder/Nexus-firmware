// §31 mandatory scenario tests. Each exercises one required failure/recovery behavior end to end
// through the real modules (stub HAL), verifying the system reacts as specified.

#include <gtest/gtest.h>
#include <sodium.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "amplifier/AmplifierManager.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioPacket.h"
#include "audio/AudioReceiver.h"
#include "audio/IAudioOutputHal.h"
#include "audio/PlaybackManager.h"
#include "audio/StreamSync.h"
#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "dsp/DspEngine.h"
#include "identity/DeviceIdentity.h"
#include "storage/SecureStorage.h"
#include "system/SystemManager.h"

namespace fs = std::filesystem;
using namespace nexus;

namespace {
fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_scn_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
audio::AudioPacket tone(std::uint64_t seq, int frames = 480) {
  audio::AudioPacket p;
  p.header.sequence = seq;
  p.header.frame_count = static_cast<std::uint32_t>(frames);
  p.samples.assign(static_cast<std::size_t>(frames) * 2, 2000);
  return p;
}
}  // namespace

// Boot Test: a fresh device reaches a sane state and provisions identity.
TEST(Scenario, Boot) {
  auto dir = sandbox("boot");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  system::SystemManager sys(&bus);
  ASSERT_TRUE(sys.start().ok());
  ASSERT_TRUE(cfg.start().ok());
  ASSERT_TRUE(id.start().ok());
  sys.enterInitialState(cfg.get().pairing.paired);
  EXPECT_EQ(sys.states().current(), system::SystemState::Unconfigured);
  EXPECT_FALSE(id.deviceId().empty());
  sys.stop();
}

// Network Disconnect Test: losing the network moves the system Offline; reconnect resumes search.
TEST(Scenario, NetworkDisconnect) {
  core::EventBus bus;
  system::SystemManager sys(&bus);
  sys.start();
  sys.enterInitialState(true);
  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});
  bus.publish(core::Event{core::EventType::StreamerFound, "d"});
  bus.publish(core::Event{core::EventType::PairingCompleted, "p"});
  bus.drain();
  ASSERT_EQ(sys.states().current(), system::SystemState::Online);

  bus.publish(core::Event{core::EventType::NetworkDisconnected, "n"});
  bus.drain();
  EXPECT_EQ(sys.states().current(), system::SystemState::Offline);

  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});
  bus.drain();
  EXPECT_EQ(sys.states().current(), system::SystemState::SearchingStreamer);
  sys.stop();
}

// Streamer Disconnect Test: losing the streamer moves the system Offline.
TEST(Scenario, StreamerDisconnect) {
  core::EventBus bus;
  system::SystemManager sys(&bus);
  sys.start();
  sys.enterInitialState(true);
  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});
  bus.publish(core::Event{core::EventType::StreamerFound, "d"});
  bus.publish(core::Event{core::EventType::PairingCompleted, "p"});
  bus.drain();
  ASSERT_EQ(sys.states().current(), system::SystemState::Online);

  bus.publish(core::Event{core::EventType::StreamerDisconnected, "d"});
  bus.drain();
  EXPECT_EQ(sys.states().current(), system::SystemState::Offline);
  sys.stop();
}

// Audio Packet Loss Test: dropped packets are counted and playback continues (no crash).
TEST(Scenario, AudioPacketLoss) {
  core::EventBus bus;
  audio::AudioBuffer buffer(2, 50);
  audio::StreamSync sync;
  double now = 1000.0;
  auto src = std::make_unique<audio::StubAudioSource>();
  auto* src_ptr = src.get();
  audio::AudioReceiver rx(&bus, &buffer, &sync, [&] { return now; }, std::move(src));
  rx.start();

  // Inject a stream with a gap: 1,2,3, [4,5 lost], 6,7,8.
  for (std::uint64_t seq : {1, 2, 3, 6, 7, 8}) {
    src_ptr->inject(audio::packPacket(tone(seq)));
  }
  bus.drain();
  EXPECT_EQ(buffer.metrics().lost, 2u);   // 4 and 5 detected as lost
  EXPECT_TRUE(rx.streaming());            // stream keeps going

  // Playback drains what arrived without crashing.
  auto dir = sandbox("loss");
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  cfg.start();
  audio::StubAudioOutputHal out;
  audio::PlaybackManager pb(&buffer, &sync, &out, &cfg, [&] { return now; });
  pb.open(48000, 2);
  for (int i = 0; i < 6; ++i) pb.step();
  EXPECT_GT(pb.framesPlayed(), 0u);
  rx.stop();
}

// Power Failure Test: an unclean shutdown (no graceful stop) must not corrupt persistent state —
// on the next boot the device loads its identity and a valid config.
TEST(Scenario, PowerFailureRecovery) {
  auto dir = sandbox("power");
  std::string dev_id;
  {
    core::EventBus bus;
    storage::SecureStorage secrets((dir / "secure").string());
    identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
    config::ConfigManager cfg((dir / "config.json").string(), &bus);
    id.start();
    cfg.start();
    cfg.update([](config::SpeakerConfig& c) { c.audio.volume = 66; });
    dev_id = id.deviceId();
    // Simulate power loss: destructors run but we never called a graceful shutdown/flush beyond
    // the atomic writes already performed.
  }
  // Reboot.
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  ASSERT_TRUE(id.start().ok());
  ASSERT_TRUE(cfg.start().ok());
  EXPECT_EQ(id.deviceId(), dev_id);          // identity intact
  EXPECT_EQ(cfg.get().audio.volume, 66);     // last persisted config intact
}

// DSP Failure Test: bypassing/failing the DSP must not crash playback — audio still flows.
TEST(Scenario, DspFailureDegradesGracefully) {
  core::EventBus bus;
  dsp::DspEngine dsp(&bus, 48000.0, 2);
  // Simulate a DSP fault by bypassing it; playback should pass audio through unchanged.
  dsp.setBypass(true);
  std::vector<std::int16_t> pcm(960, 1500);
  auto copy = pcm;
  dsp.processInt16(pcm.data(), 480, 2);
  EXPECT_EQ(pcm, copy);  // no processing, no crash
}

// Amplifier Overheat Test: crossing the shutdown threshold mutes the amp and alerts.
TEST(Scenario, AmplifierOverheat) {
  core::EventBus bus;
  auto hal = std::make_unique<amplifier::StubAmplifierHal>();
  auto* hal_ptr = hal.get();
  amplifier::AmplifierManager amp(&bus, std::move(hal), 70.0, 85.0);
  amp.start();
  amp.unmute();
  std::atomic<int> overheat{0};
  bus.subscribe(core::EventType::AmplifierOverheat, [&](const core::Event&) { ++overheat; });
  hal_ptr->temperature = 95.0;
  amp.poll();
  bus.drain();
  EXPECT_EQ(amp.ampState(), amplifier::AmpState::Overheated);
  EXPECT_TRUE(hal_ptr->muted);
  EXPECT_EQ(overheat.load(), 1);
  amp.stop();
}

// Factory Reset Test: reset clears config but preserves the permanent identity + keypair.
TEST(Scenario, FactoryResetPreservesIdentity) {
  auto dir = sandbox("reset");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  id.start();
  cfg.start();
  cfg.update([](config::SpeakerConfig& c) {
    c.pairing.paired = true;
    c.pairing.streamer_id = "STR-9";
  });
  const std::string dev_id = id.deviceId();

  // Factory reset: config back to defaults (identity dir + secure keypair untouched).
  ASSERT_TRUE(cfg.resetToDefaults().ok());
  EXPECT_FALSE(cfg.get().pairing.paired);
  EXPECT_TRUE(fs::exists((dir / "identity/factory.json")));
  EXPECT_TRUE(secrets.has(identity::KeyManager::kSecretKeyName));

  // Same identity after reset.
  identity::DeviceIdentity id2((dir / "identity/factory.json").string(), &secrets, &bus);
  ASSERT_TRUE(id2.start().ok());
  EXPECT_EQ(id2.deviceId(), dev_id);
}
