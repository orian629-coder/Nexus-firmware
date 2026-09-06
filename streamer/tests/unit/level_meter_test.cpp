// Signal metering: the input/output VU readings the UI shows.
//
// The point of these tests is that a meter must never LIE — a bar that reads healthy while the
// stream is dead is worse than no meter at all, because it sends you looking for the fault in the
// wrong place.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "audio/LevelMeter.h"

using nexus::streamer::audio::dbToFraction;
using nexus::streamer::audio::LevelMeter;
using nexus::streamer::audio::toDbfs;

namespace {

// Interleaved stereo sine at a given amplitude (0..32767).
std::vector<std::int16_t> tone(double amplitude, std::size_t frames = 480) {
  std::vector<std::int16_t> pcm(frames * 2);
  for (std::size_t f = 0; f < frames; ++f) {
    const double s = std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(f) / 48000.0) * amplitude;
    pcm[f * 2] = pcm[f * 2 + 1] = static_cast<std::int16_t>(s);
  }
  return pcm;
}

}  // namespace

TEST(LevelMeter, StartsSilentAndInactive) {
  LevelMeter m;
  const auto v = m.read();
  EXPECT_FLOAT_EQ(v.rms_left, 0.0f);
  EXPECT_FLOAT_EQ(v.peak_left, 0.0f);
  EXPECT_FALSE(v.active) << "nothing has been measured yet";
}

TEST(LevelMeter, SilenceReadsAsSilenceNotAsSignal) {
  LevelMeter m;
  std::vector<std::int16_t> quiet(480 * 2, 0);
  m.measure(quiet.data(), 480, 2);

  const auto v = m.read();
  EXPECT_FLOAT_EQ(v.rms_left, 0.0f);
  EXPECT_FLOAT_EQ(v.peak_left, 0.0f);
  EXPECT_TRUE(v.active) << "a measured block of silence is still a live stream";
  // Digital silence has no logarithm; it must floor, never produce -inf/NaN, which would serialize
  // as invalid JSON and blank the meter in the browser.
  EXPECT_DOUBLE_EQ(toDbfs(v.rms_left), -60.0);
  EXPECT_DOUBLE_EQ(dbToFraction(toDbfs(v.rms_left)), 0.0);
}

TEST(LevelMeter, LouderSignalReadsHigher) {
  LevelMeter quiet_meter, loud_meter;
  const auto quiet = tone(1000.0);
  const auto loud = tone(20000.0);

  quiet_meter.measure(quiet.data(), 480, 2);
  loud_meter.measure(loud.data(), 480, 2);

  EXPECT_GT(loud_meter.read().rms_left, quiet_meter.read().rms_left);
  EXPECT_GT(toDbfs(loud_meter.read().rms_left), toDbfs(quiet_meter.read().rms_left));
}

// A full-scale sine has RMS = amplitude/sqrt(2) ≈ 0.707. Checking the actual number (not just
// "greater than") is what catches a meter that is scaled wrong and shows every signal as clipping.
TEST(LevelMeter, RmsMatchesTheKnownValueForASine) {
  LevelMeter m;
  const auto full = tone(32767.0, 4800);
  m.measure(full.data(), 4800, 2);

  const auto v = m.read();
  EXPECT_NEAR(v.rms_left, 0.707f, 0.01f);
  EXPECT_NEAR(v.peak_left, 1.0f, 0.01f);
  // ≈ -3 dBFS for a full-scale sine.
  EXPECT_NEAR(toDbfs(v.rms_left), -3.0, 0.3);
}

// Peak is the only reading that reveals clipping — RMS stays comfortably mid-scale even when
// samples are pinned at full scale.
TEST(LevelMeter, PeakRevealsClippingThatRmsHides) {
  LevelMeter m;
  std::vector<std::int16_t> square(480 * 2);
  for (std::size_t i = 0; i < square.size(); ++i) {
    square[i] = (i / 2) % 2 == 0 ? 32767 : -32768;
  }
  m.measure(square.data(), 480, 2);

  const auto v = m.read();
  EXPECT_GE(v.peak_left, 0.999f) << "full-scale samples must register as peak 1.0";
  EXPECT_LT(v.rms_left, 1.01f);
}

TEST(LevelMeter, ResetDropsTheBarsAndMarksInactive) {
  LevelMeter m;
  const auto loud = tone(20000.0);
  m.measure(loud.data(), 480, 2);
  ASSERT_GT(m.read().rms_left, 0.0f);

  m.reset();

  const auto v = m.read();
  EXPECT_FLOAT_EQ(v.rms_left, 0.0f);
  EXPECT_FLOAT_EQ(v.peak_left, 0.0f);
  EXPECT_FALSE(v.active) << "a stopped stream must not leave the meter frozen at its last reading";
}

// Mono sources must not leave the right bar dead.
TEST(LevelMeter, MonoIsMirroredIntoBothChannels) {
  LevelMeter m;
  std::vector<std::int16_t> mono(480);
  for (std::size_t f = 0; f < mono.size(); ++f) {
    mono[f] = static_cast<std::int16_t>(std::sin(2.0 * M_PI * 1000.0 * f / 48000.0) * 10000.0);
  }
  m.measure(mono.data(), 480, 1);

  const auto v = m.read();
  EXPECT_GT(v.rms_left, 0.0f);
  EXPECT_FLOAT_EQ(v.rms_left, v.rms_right);
}

TEST(LevelMeter, IgnoresDegenerateBlocksInsteadOfCrashing) {
  LevelMeter m;
  const auto t = tone(10000.0);
  EXPECT_NO_THROW({
    m.measure(nullptr, 480, 2);
    m.measure(t.data(), 0, 2);
    m.measure(t.data(), 480, 0);
  });
  EXPECT_FALSE(m.read().active) << "a degenerate block must not be mistaken for a measurement";
}

// ── scale conversion ──

TEST(LevelMeterScale, DbToFractionSpansTheBar) {
  EXPECT_DOUBLE_EQ(dbToFraction(0.0), 1.0);      // full scale → full bar
  EXPECT_DOUBLE_EQ(dbToFraction(-60.0), 0.0);    // floor → empty
  EXPECT_DOUBLE_EQ(dbToFraction(-30.0), 0.5);    // midpoint
  EXPECT_DOUBLE_EQ(dbToFraction(-90.0), 0.0);    // below floor clamps, never goes negative
  EXPECT_DOUBLE_EQ(dbToFraction(6.0), 1.0);      // above full scale clamps
}
