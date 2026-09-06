#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "audio/AudioPacket.h"
#include "send/MemoryPacketSink.h"
#include "send/Packetizer.h"
#include "send/StreamSender.h"
#include "send/Timestamper.h"
#include "sources/MemoryAudioSource.h"
#include "sources/StdinPcmSource.h"

#include <cstdio>

using namespace nexus::streamer;

namespace {
constexpr int kRate = nexus::audio::wire::kSampleRate;   // 48000
constexpr int kCh = nexus::audio::wire::kChannels;       // 2
constexpr std::uint32_t kBlock = 480;                    // 10 ms

// Build a deterministic interleaved-stereo ramp of `frames` frames.
std::vector<std::int16_t> makeRamp(std::size_t frames) {
  std::vector<std::int16_t> s(frames * kCh);
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<std::int16_t>((i * 7) % 1000 - 500);
  return s;
}
}  // namespace

// Packetize a stream with the streamer, decode each datagram with the SPEAKER's real unpackPacket,
// and assert: exact PCM round-trip, monotonic sequence from 0, future timestamps on the audio-clock
// cadence, and kFlagLast only on the final packet. This is the Phase-1 wire-contract proof.
TEST(SendPipeline, RoundTripsThroughSpeakerUnpack) {
  const std::size_t total_frames = kBlock * 3 + 120;  // 3 full blocks + a short final block
  const auto pcm = makeRamp(total_frames);

  sources::MemoryAudioSource src(pcm, kCh);
  send::Timestamper ts(kRate, kBlock, /*lead=*/0.18);
  send::Packetizer pk(ts, kCh);
  send::MemoryPacketSink sink;
  send::StreamSender sender(src, ts, pk, sink, kCh, kBlock);

  const double t0 = 1785100000.0;
  const std::size_t sent = sender.runToEnd(t0);
  ASSERT_EQ(sent, 4u);
  ASSERT_EQ(sink.datagrams().size(), 4u);

  const double block_secs = static_cast<double>(kBlock) / kRate;
  std::vector<std::int16_t> reassembled;
  for (std::size_t i = 0; i < sink.datagrams().size(); ++i) {
    nexus::audio::AudioPacket out;
    ASSERT_TRUE(nexus::audio::unpackPacket(sink.datagrams()[i].data(), sink.datagrams()[i].size(),
                                           kCh, out));
    EXPECT_EQ(out.header.sequence, i);
    EXPECT_DOUBLE_EQ(out.header.timestamp, t0 + 0.18 + static_cast<double>(i) * block_secs);
    EXPECT_GT(out.header.timestamp, t0);  // always stamped in the future
    const bool is_last = (i + 1 == sink.datagrams().size());
    EXPECT_EQ(out.isLast(), is_last);
    reassembled.insert(reassembled.end(), out.samples.begin(), out.samples.end());
  }
  EXPECT_EQ(reassembled, pcm);  // exact sample-for-sample round-trip
}

// StdinPcmSource reads raw s16le PCM from a FILE* and yields whole frames; the same bytes come back
// out through the packetizer/speaker-decode path — proving the live-capture pipe is contract-exact.
TEST(StdinPcmSource, ReadsWholeFramesAndRoundTrips) {
  const std::size_t total_frames = kBlock + 37;  // one full block + a short tail
  const auto pcm = makeRamp(total_frames);

  // Write the PCM to a temp file and reopen it as the "stdin" FILE*.
  std::FILE* tmp = std::tmpfile();
  ASSERT_NE(tmp, nullptr);
  ASSERT_EQ(std::fwrite(pcm.data(), sizeof(std::int16_t), pcm.size(), tmp), pcm.size());
  std::rewind(tmp);

  sources::StdinPcmSource src(tmp, kCh);
  send::Timestamper ts(kRate, kBlock, 0.18);
  send::Packetizer pk(ts, kCh);
  send::MemoryPacketSink sink;
  send::StreamSender sender(src, ts, pk, sink, kCh, kBlock);

  const std::size_t sent = sender.runToEnd(1000.0);
  EXPECT_EQ(sent, 2u);  // full block + short tail
  EXPECT_TRUE(src.exhausted());

  std::vector<std::int16_t> reassembled;
  for (const auto& dg : sink.datagrams()) {
    nexus::audio::AudioPacket out;
    ASSERT_TRUE(nexus::audio::unpackPacket(dg.data(), dg.size(), kCh, out));
    reassembled.insert(reassembled.end(), out.samples.begin(), out.samples.end());
  }
  EXPECT_EQ(reassembled, pcm);
  std::fclose(tmp);
}

TEST(SendPipeline, ExactBlockMultipleHasNoShortTail) {
  const std::size_t total_frames = kBlock * 2;
  const auto pcm = makeRamp(total_frames);
  sources::MemoryAudioSource src(pcm, kCh);
  send::Timestamper ts(kRate, kBlock, 0.18);
  send::Packetizer pk(ts, kCh);
  send::MemoryPacketSink sink;
  send::StreamSender sender(src, ts, pk, sink, kCh, kBlock);

  EXPECT_EQ(sender.runToEnd(1000.0), 2u);
  nexus::audio::AudioPacket out;
  ASSERT_TRUE(nexus::audio::unpackPacket(sink.datagrams().back().data(),
                                         sink.datagrams().back().size(), kCh, out));
  EXPECT_TRUE(out.isLast());
  EXPECT_EQ(out.header.frame_count, kBlock);
}
