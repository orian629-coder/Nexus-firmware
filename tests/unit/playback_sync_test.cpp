// Timestamp-honouring playback (Phase 6).
//
// Every speaker receives the same absolute target timestamp and must release that packet when its
// own clock reaches it. Playing on arrival instead — the previous behaviour — leaves each speaker
// running on its own network jitter, and no amount of buffering makes two of them line up.
//
// These tests drive PlaybackManager with an injected clock, so "wait until due" is verified
// deterministically rather than by sleeping.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "audio/AudioBuffer.h"
#include "audio/PlaybackManager.h"
#include "config/ConfigManager.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

// Records what actually reached the output device, so a test can tell "played" from "held back".
class RecordingOutput : public audio::IAudioOutputHal {
 public:
  core::Status open(int, int) override { return core::Status::success(); }
  core::Status close() override { return core::Status::success(); }
  core::Result<std::size_t> write(const std::int16_t* pcm, std::size_t frames) override {
    written.insert(written.end(), pcm, pcm + frames * 2);
    ++writes;
    return frames;
  }

  std::vector<std::int16_t> written;
  int writes = 0;
};

audio::AudioPacket packetAt(double timestamp, std::uint64_t seq, std::int16_t value) {
  audio::AudioPacket p;
  p.header.timestamp = timestamp;
  p.header.sequence = seq;
  p.header.frame_count = 2;
  p.samples = {value, value, value, value};  // 2 frames, stereo
  return p;
}

// Config on a scratch path; volume 100 / unmuted so gain never masks what playback did.
struct Cfg {
  fs::path dir;
  std::unique_ptr<config::ConfigManager> mgr;

  explicit Cfg(const std::string& name) {
    dir = fs::temp_directory_path() / ("nexus-sync-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    mgr = std::make_unique<config::ConfigManager>((dir / "config.json").string());
    mgr->load();
    mgr->update([](config::SpeakerConfig& c) {
      c.audio.volume = 100;
      c.audio.muted = false;
    });
  }
  ~Cfg() { fs::remove_all(dir); }
};

}  // namespace

// The core behaviour: a packet stamped for the future is NOT played early.
TEST(PlaybackSync, HoldsAPacketUntilItsTimestampArrives) {
  audio::AudioBuffer buffer(/*prefill=*/1);
  audio::StreamSync sync(/*target_buffer_ms=*/0.0);  // no early-release margin, so timing is exact
  RecordingOutput out;
  Cfg cfg("hold");
  double now = 1000.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  buffer.push(packetAt(/*timestamp=*/1005.0, 1, 500));

  // Well before the target: nothing may be emitted.
  EXPECT_EQ(pb.step(), 0u);
  EXPECT_EQ(out.writes, 0) << "a packet stamped 5s in the future must not play now";

  // One millisecond early is still early.
  now = 1004.999;
  EXPECT_EQ(pb.step(), 0u);
  EXPECT_EQ(out.writes, 0);

  // At the timestamp it plays.
  now = 1005.0;
  EXPECT_EQ(pb.step(), 2u);
  EXPECT_EQ(out.writes, 1);
  EXPECT_EQ(out.written.front(), 500);
}

// Holding back must not consume the packet: the same one plays once it is due.
TEST(PlaybackSync, AHeldPacketIsNotDiscarded) {
  audio::AudioBuffer buffer(1);
  audio::StreamSync sync(0.0);
  RecordingOutput out;
  Cfg cfg("keep");
  double now = 100.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  buffer.push(packetAt(105.0, 1, 777));
  for (int i = 0; i < 20; ++i) EXPECT_EQ(pb.step(), 0u);
  EXPECT_EQ(buffer.depth(), 1u) << "peeking must not remove the packet";

  now = 105.0;
  EXPECT_EQ(pb.step(), 2u);
  EXPECT_EQ(out.written.front(), 777);
  EXPECT_EQ(buffer.depth(), 0u);
}

// Repeatedly checking a not-yet-due packet must not be counted as underflow: the buffer is not
// empty, we are simply early. Miscounting here would make a healthy stream look broken in
// telemetry.
TEST(PlaybackSync, WaitingIsNotCountedAsUnderflow) {
  audio::AudioBuffer buffer(1);
  audio::StreamSync sync(0.0);
  RecordingOutput out;
  Cfg cfg("underflow");
  double now = 0.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  buffer.push(packetAt(10.0, 1, 100));
  for (int i = 0; i < 50; ++i) pb.step();

  EXPECT_EQ(buffer.metrics().underflows, 0u)
      << "waiting for a future packet is not the same as having none";
}

// A late packet plays immediately — catching up beats adding more delay.
TEST(PlaybackSync, LatePacketsPlayImmediately) {
  audio::AudioBuffer buffer(1);
  audio::StreamSync sync(0.0);
  RecordingOutput out;
  Cfg cfg("late");
  double now = 500.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  buffer.push(packetAt(/*timestamp=*/499.0, 1, 321));  // one second late
  EXPECT_EQ(pb.step(), 2u);
  EXPECT_EQ(out.writes, 1);
}

// An unstamped packet (timestamp 0) is played rather than held forever waiting for a moment that
// never comes. Older senders and test sources produce these.
TEST(PlaybackSync, UnstampedPacketsPlayImmediately) {
  audio::AudioBuffer buffer(1);
  audio::StreamSync sync(0.0);
  RecordingOutput out;
  Cfg cfg("unstamped");
  double now = 9999.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  buffer.push(packetAt(/*timestamp=*/0.0, 1, 42));
  EXPECT_EQ(pb.step(), 2u) << "a packet with no timestamp must not be held forever";
}

// The alignment property itself: two independent speakers fed the same stamped packets release
// them at the same clock reading, even when one received its packet much earlier than the other.
TEST(PlaybackSync, TwoSpeakersReleaseTheSameSampleAtTheSameTime) {
  audio::AudioBuffer buf_a(1), buf_b(1);
  audio::StreamSync sync_a(0.0), sync_b(0.0);
  RecordingOutput out_a, out_b;
  Cfg cfg("align");
  double now = 200.0;
  audio::PlaybackManager a(&buf_a, &sync_a, &out_a, cfg.mgr.get(), [&] { return now; });
  audio::PlaybackManager b(&buf_b, &sync_b, &out_b, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(a.open(48000, 2).ok());
  ASSERT_TRUE(b.open(48000, 2).ok());

  const double target = 203.0;
  // Speaker A receives the packet 3 s ahead of time; speaker B receives it barely in time. Under
  // play-on-arrival this alone would put them seconds apart.
  buf_a.push(packetAt(target, 1, 900));

  for (int i = 0; i < 10; ++i) {  // A waits instead of racing ahead
    EXPECT_EQ(a.step(), 0u);
  }
  now = 202.999;
  buf_b.push(packetAt(target, 1, 900));
  EXPECT_EQ(a.step(), 0u);
  EXPECT_EQ(b.step(), 0u);

  now = target;
  EXPECT_EQ(a.step(), 2u);
  EXPECT_EQ(b.step(), 2u);
  EXPECT_EQ(out_a.writes, 1);
  EXPECT_EQ(out_b.writes, 1);
  EXPECT_EQ(out_a.written, out_b.written) << "both speakers must emit the same samples";
}

// The target buffer releases packets slightly early to cover output latency, which is what keeps
// the DAC fed. Verify that margin is actually honoured.
TEST(PlaybackSync, TargetBufferReleasesEarlyByThatMargin) {
  audio::AudioBuffer buffer(1);
  audio::StreamSync sync(/*target_buffer_ms=*/80.0);
  RecordingOutput out;
  Cfg cfg("margin");
  double now = 0.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  buffer.push(packetAt(/*timestamp=*/1.0, 1, 55));

  now = 0.919;  // 81 ms early — still too early
  EXPECT_EQ(pb.step(), 0u);

  now = 0.920;  // exactly 80 ms early — release now
  EXPECT_EQ(pb.step(), 2u);
}

// ── the safety valve, found on hardware ──
//
// Waiting for a timestamp only works while the queue can hold the audio being held back. With a
// sender lead of 2 s against a 0.5 s buffer, the speaker held the head packet while the tail
// overflowed: 3779 packets dropped and not one sample played. Losing alignment is recoverable;
// destroying the stream is not.
TEST(PlaybackSync, PlaysEarlyRatherThanOverflowingTheBuffer) {
  audio::AudioBuffer buffer(/*prefill=*/1, /*capacity=*/10);
  audio::StreamSync sync(0.0);
  RecordingOutput out;
  Cfg cfg("valve");
  double now = 0.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  // Every packet is stamped far in the future, so without the valve playback would hold all of them.
  for (std::uint64_t i = 0; i < 8; ++i) {
    buffer.push(packetAt(/*timestamp=*/1000.0 + static_cast<double>(i), i + 1, 111));
  }

  // 8/10 is at the 80% threshold: release rather than keep waiting.
  EXPECT_EQ(pb.step(), 2u) << "a nearly-full buffer must drain, not hold until it overflows";
  EXPECT_EQ(out.writes, 1);
}

TEST(PlaybackSync, StillWaitsWhileTheBufferHasRoom) {
  audio::AudioBuffer buffer(/*prefill=*/1, /*capacity=*/10);
  audio::StreamSync sync(0.0);
  RecordingOutput out;
  Cfg cfg("room");
  double now = 0.0;
  audio::PlaybackManager pb(&buffer, &sync, &out, cfg.mgr.get(), [&] { return now; });
  ASSERT_TRUE(pb.open(48000, 2).ok());

  // Well under the threshold — the valve must not fire, or sync would never hold anything.
  buffer.push(packetAt(1000.0, 1, 222));
  buffer.push(packetAt(1000.01, 2, 222));
  EXPECT_EQ(pb.step(), 0u) << "with room to spare, a future packet must still wait";
  EXPECT_EQ(out.writes, 0);
}

TEST(BufferNearlyFull, TripsAtEightyPercentOfCapacity) {
  audio::AudioBuffer buffer(/*prefill=*/1, /*capacity=*/10);
  EXPECT_FALSE(buffer.nearlyFull()) << "empty";

  for (std::uint64_t i = 0; i < 7; ++i) buffer.push(packetAt(1.0, i + 1, 0));
  EXPECT_FALSE(buffer.nearlyFull()) << "7/10 is below the threshold";

  buffer.push(packetAt(1.0, 8, 0));
  EXPECT_TRUE(buffer.nearlyFull()) << "8/10 = 80% must trip the valve";
}

// ── AudioBuffer::peekTimestamp ──

TEST(BufferPeek, ReturnsFrontTimestampWithoutRemovingIt) {
  audio::AudioBuffer buffer(1);
  buffer.push(packetAt(42.5, 1, 10));
  buffer.push(packetAt(43.5, 2, 20));

  ASSERT_TRUE(buffer.peekTimestamp().has_value());
  EXPECT_DOUBLE_EQ(*buffer.peekTimestamp(), 42.5) << "must report the FRONT packet";
  EXPECT_EQ(buffer.depth(), 2u) << "peek must not consume";
}

TEST(BufferPeek, EmptyBufferPeekIsNotAnUnderflow) {
  audio::AudioBuffer buffer(1);
  EXPECT_FALSE(buffer.peekTimestamp().has_value());
  EXPECT_EQ(buffer.metrics().underflows, 0u)
      << "peeking is a question, not a read; only pop() counts as underflow";
}
