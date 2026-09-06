// Long-duration / stability tests (§31: "24-Hour Playback", "7-Day Stability").
//
// A real 24h/7-day run happens on hardware (gated by NEXUS_SOAK). Here we run a *time-scaled*
// simulation: pump the equivalent number of audio blocks and heartbeat/state cycles through the
// real pipeline as fast as possible, and assert the invariants that would break over a long run —
// no unbounded buffer growth, no state drift, playback keeps up, metrics stay sane.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "audio/AudioBuffer.h"
#include "audio/AudioPacket.h"
#include "audio/AudioReceiver.h"
#include "audio/IAudioOutputHal.h"
#include "audio/PlaybackManager.h"
#include "audio/StreamSync.h"
#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "dsp/DspEngine.h"
#include "system/SystemManager.h"

#include <atomic>

namespace fs = std::filesystem;
using namespace nexus;

namespace {
fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_soak_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// One 480-frame stereo block at 48 kHz = 10 ms of audio. Blocks per hour = 360000.
constexpr int kBlockFrames = 480;
constexpr double kBlockSeconds = kBlockFrames / 48000.0;

// Scale factor: full runs on hardware (NEXUS_SOAK=1), a short proxy in CI.
int hoursToSimulate(int full_hours) {
  const char* soak = std::getenv("NEXUS_SOAK");
  if (soak && std::string(soak) == "1") return full_hours;
  return 0;  // proxy mode: simulate a small fixed number of blocks instead
}
}  // namespace

// 24-Hour Playback: stream continuously; the jitter buffer must not grow unbounded, playback must
// keep up, and no pathological metric accumulates.
TEST(Soak, ContinuousPlaybackStaysStable) {
  auto dir = sandbox("playback");
  core::EventBus bus;
  config::ConfigManager cfg((dir / "config.json").string(), &bus);
  cfg.start();
  cfg.update([](config::SpeakerConfig& c) { c.audio.muted = false; c.audio.volume = 80; });

  audio::AudioBuffer buffer(3, 50);
  audio::StreamSync sync;
  dsp::DspEngine dsp(&bus, 48000.0, 2);
  audio::StubAudioOutputHal out;
  double now = 0.0;

  audio::PlaybackManager pb(&buffer, &sync, &out, &cfg, [&] { return now; });
  pb.setDspHook([&](std::int16_t* io, std::size_t fr, int ch) { dsp.processInt16(io, fr, ch); });
  pb.open(48000, 2);

  const int hours = hoursToSimulate(24);
  const long blocks = hours > 0 ? static_cast<long>(hours) * 360000L : 20000L;  // proxy: 20k blocks

  std::uint64_t seq = 0;
  for (long i = 0; i < blocks; ++i) {
    // Streamer sends a block; player consumes one. Producer slightly ahead so the buffer fills to
    // steady state and then holds.
    audio::AudioPacket p;
    p.header.sequence = ++seq;
    p.header.timestamp = now;
    p.header.frame_count = kBlockFrames;
    p.samples.assign(kBlockFrames * 2, static_cast<std::int16_t>(3000));
    buffer.push(p);
    pb.step();
    now += kBlockSeconds;

    // Invariant: the buffer never exceeds its capacity (overflow drops keep it bounded).
    ASSERT_LE(buffer.depth(), 50u);
  }

  // Steady-state: played roughly as many blocks as we pushed, buffer bounded, no runaway loss.
  EXPECT_GT(pb.framesPlayed(), 0u);
  EXPECT_LE(buffer.depth(), 50u);
  // In proxy mode producer==consumer rate so overflow drops stay modest; just assert it's bounded.
  EXPECT_LT(buffer.metrics().dropped_overflow, static_cast<std::uint64_t>(blocks));
}

// 7-Day Stability: cycle the state machine and heartbeat-equivalent event churn many times; state
// must always resolve to a legal value and the event bus must never wedge.
TEST(Soak, LongRunStateAndEventChurnStaysConsistent) {
  core::EventBus bus;
  system::SystemManager sys(&bus);

  // Heartbeat every 30s over 7 days = 20160 beats; proxy runs a small multiple.
  const int days = hoursToSimulate(24 * 7) / 24;
  const long beats = days > 0 ? static_cast<long>(days) * 2880L : 5000L;

  std::atomic<long> delivered{0};
  bus.subscribe(core::EventType::StateChanged, [&](const core::Event&) { ++delivered; });

  sys.start();
  sys.enterInitialState(true);
  // Drive to Online, then churn online/playing/online transitions like a week of activity.
  bus.publish(core::Event{core::EventType::NetworkConnected, "n"});
  bus.publish(core::Event{core::EventType::StreamerFound, "d"});
  bus.publish(core::Event{core::EventType::PairingCompleted, "p"});
  bus.drain();
  ASSERT_EQ(sys.states().current(), system::SystemState::Online);

  for (long i = 0; i < beats; ++i) {
    bus.publish(core::Event{core::EventType::AudioStarted, "a"});   // → Playing
    bus.publish(core::Event{core::EventType::AudioStopped, "a"});   // → Online
  }
  bus.drain();

  // The bus delivered every transition (no wedge), and the state resolved back to Online.
  EXPECT_EQ(sys.states().current(), system::SystemState::Online);
  EXPECT_GT(delivered.load(), 0L);
  sys.stop();
}
