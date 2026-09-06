#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>

#include "audio/AudioBuffer.h"
#include "audio/AudioPacket.h"
#include "audio/AudioReceiver.h"
#include "audio/IAudioOutputHal.h"
#include "audio/PlaybackEngine.h"
#include "audio/PlaybackManager.h"
#include "audio/StreamSync.h"
#include "config/ConfigManager.h"
#include "core/EventBus.h"

#include <chrono>
#include <thread>

using namespace nexus;
using namespace nexus::audio;
namespace fs = std::filesystem;

namespace {
AudioPacket makePacket(std::uint64_t seq, double ts, int frames, int channels = 2,
                       std::int16_t value = 1000) {
  AudioPacket p;
  p.header.sequence = seq;
  p.header.timestamp = ts;
  p.header.frame_count = static_cast<std::uint32_t>(frames);
  p.samples.assign(static_cast<std::size_t>(frames * channels), value);
  return p;
}
}  // namespace

TEST(AudioPacket, PackUnpackRoundTrip) {
  auto p = makePacket(7, 1234.5, 4, 2, 555);
  auto bytes = packPacket(p);
  AudioPacket out;
  ASSERT_TRUE(unpackPacket(bytes.data(), bytes.size(), 2, out));
  EXPECT_EQ(out.header.sequence, 7u);
  EXPECT_DOUBLE_EQ(out.header.timestamp, 1234.5);
  EXPECT_EQ(out.header.frame_count, 4u);
  EXPECT_EQ(out.samples, p.samples);
}

TEST(AudioPacket, RejectsTruncated) {
  auto p = makePacket(1, 0, 10);
  auto bytes = packPacket(p);
  bytes.resize(bytes.size() - 4);  // chop off samples
  AudioPacket out;
  EXPECT_FALSE(unpackPacket(bytes.data(), bytes.size(), 2, out));
}

TEST(AudioBuffer, PrefillGate) {
  AudioBuffer buf(/*prefill*/ 3, /*capacity*/ 10);
  EXPECT_FALSE(buf.ready());
  buf.push(makePacket(1, 0, 2));
  buf.push(makePacket(2, 0, 2));
  EXPECT_FALSE(buf.ready());
  buf.push(makePacket(3, 0, 2));
  EXPECT_TRUE(buf.ready());
}

TEST(AudioBuffer, DetectsPacketLossFromSequenceGap) {
  AudioBuffer buf;
  buf.push(makePacket(1, 0, 2));
  buf.push(makePacket(2, 0, 2));
  buf.push(makePacket(5, 0, 2));  // skipped 3,4
  EXPECT_EQ(buf.metrics().lost, 2u);
}

TEST(AudioBuffer, OverflowDropsOldest) {
  AudioBuffer buf(/*prefill*/ 1, /*capacity*/ 2);
  buf.push(makePacket(1, 0, 2));
  buf.push(makePacket(2, 0, 2));
  buf.push(makePacket(3, 0, 2));  // overflow -> drop seq 1
  EXPECT_EQ(buf.depth(), 2u);
  EXPECT_EQ(buf.metrics().dropped_overflow, 1u);
  auto front = buf.pop();
  ASSERT_TRUE(front.has_value());
  EXPECT_EQ(front->header.sequence, 2u);  // oldest survivor
}

TEST(AudioBuffer, UnderflowCounted) {
  AudioBuffer buf;
  EXPECT_FALSE(buf.pop().has_value());
  EXPECT_EQ(buf.metrics().underflows, 1u);
}

TEST(StreamSync, DueWhenTimestampReached) {
  StreamSync sync(/*target_buffer_ms*/ 0.0);
  EXPECT_FALSE(sync.due(/*ts*/ 100.0, /*now*/ 99.5));  // 0.5s early
  EXPECT_TRUE(sync.due(/*ts*/ 100.0, /*now*/ 100.0));
  EXPECT_TRUE(sync.due(/*ts*/ 100.0, /*now*/ 105.0));  // late -> play now
}

TEST(StreamSync, TracksOffset) {
  StreamSync sync(0.0);
  sync.observe(/*ts*/ 100.05, /*now*/ 100.0);  // packet 50ms ahead
  EXPECT_GT(sync.stats().observations, 0u);
  EXPECT_NEAR(sync.stats().clock_offset_ms, 50.0, 1.0);
}

TEST(AudioReceiver, InjectedPacketFlowsToBufferAndEmitsStarted) {
  core::EventBus bus;
  AudioBuffer buffer;
  StreamSync sync;
  std::atomic<int> started{0};
  bus.subscribe(core::EventType::AudioStarted, [&](const core::Event&) { ++started; });

  auto src = std::make_unique<StubAudioSource>();
  auto* src_ptr = src.get();
  AudioReceiver rx(&bus, &buffer, &sync, [] { return 1000.0; }, std::move(src));
  ASSERT_TRUE(rx.start().ok());

  src_ptr->inject(packPacket(makePacket(1, 1000.0, 4)));
  bus.drain();

  EXPECT_EQ(buffer.depth(), 1u);
  EXPECT_TRUE(rx.streaming());
  EXPECT_EQ(started.load(), 1);
  rx.stop();
}

TEST(PlaybackManager, PlaysAfterPrefillAndAppliesMute) {
  auto dir = fs::temp_directory_path() / "nexus_pb_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();

  AudioBuffer buffer(/*prefill*/ 2, /*capacity*/ 10);
  StreamSync sync;
  StubAudioOutputHal out;
  PlaybackManager pb(&buffer, &sync, &out, &config, [] { return 0.0; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  // Below prefill: nothing plays.
  buffer.push(makePacket(1, 0, 4));
  EXPECT_EQ(pb.step(), 0u);

  // Reach prefill: plays.
  buffer.push(makePacket(2, 0, 4));
  EXPECT_GT(pb.step(), 0u);
  EXPECT_GT(pb.framesPlayed(), 0u);

  // Mute zeroes the samples but still consumes/plays frames.
  config.update([](config::SpeakerConfig& c) { c.audio.muted = true; });
  buffer.push(makePacket(3, 0, 4));
  EXPECT_GT(pb.step(), 0u);
}

TEST(PlaybackEngine, DrainsBufferOnItsThread) {
  auto dir = std::filesystem::temp_directory_path() / "nexus_pbeng_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();
  config.update([](config::SpeakerConfig& c) { c.audio.muted = false; c.audio.volume = 100; });

  AudioBuffer buffer(2, 50);
  StreamSync sync;
  // Use an explicit stub output so we can run the real PlaybackEngine loop on the host.
  auto out = std::make_unique<StubAudioOutputHal>();
  PlaybackEngine engine(&bus, &buffer, &sync, &config, [] { return 0.0; }, std::move(out));
  ASSERT_TRUE(engine.start().ok());

  // Feed the buffer; the engine thread should drain and "play" it.
  for (std::uint64_t seq = 1; seq <= 20; ++seq) buffer.push(makePacket(seq, 0, 480));
  // Let the playback thread run.
  for (int i = 0; i < 50 && engine.framesPlayed() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

  EXPECT_GT(engine.framesPlayed(), 0u);
  engine.stop();
}
