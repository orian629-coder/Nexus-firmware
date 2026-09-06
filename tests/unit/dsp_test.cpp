#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "core/EventBus.h"
#include "dsp/Biquad.h"
#include "dsp/Delay.h"
#include "dsp/DspEngine.h"
#include "dsp/Equalizer.h"
#include "dsp/GainControl.h"
#include "dsp/Limiter.h"

using namespace nexus::dsp;

namespace {
// RMS of a float buffer.
double rms(const std::vector<float>& v) {
  double s = 0;
  for (float x : v) s += static_cast<double>(x) * x;
  return std::sqrt(s / std::max<std::size_t>(1, v.size()));
}
// Generate an interleaved stereo sine at `freq`.
std::vector<float> sine(double freq, double fs, std::size_t frames, float amp = 0.5f) {
  std::vector<float> v(frames * 2);
  for (std::size_t f = 0; f < frames; ++f) {
    float s = amp * static_cast<float>(std::sin(2.0 * M_PI * freq * f / fs));
    v[f * 2] = s;
    v[f * 2 + 1] = s;
  }
  return v;
}
}  // namespace

TEST(Biquad, PeakingBoostRaisesMagnitudeAtCenter) {
  auto c = peakingEq(1000.0, 6.0, 4.3, 48000.0);
  EXPECT_NEAR(magnitudeDb(c, 1000.0, 48000.0), 6.0, 0.5);
  // Far from center, near 0 dB.
  EXPECT_NEAR(magnitudeDb(c, 100.0, 48000.0), 0.0, 1.0);
}

TEST(Equalizer, FlatByDefaultPassesSignal) {
  Equalizer eq(48000.0, 2);
  auto buf = sine(1000.0, 48000.0, 512);
  auto before = rms(buf);
  eq.process(buf.data(), 512);
  EXPECT_NEAR(rms(buf), before, before * 0.05);  // ~unchanged when flat
}

TEST(Equalizer, BandBoostIncreasesMagnitude) {
  Equalizer eq(48000.0, 2);
  // Boost the 1 kHz region.
  eq.setBandGain(15, 8.0);  // kBandFreqs[15] == 1000 Hz
  EXPECT_GT(eq.magnitudeDb(1000.0), 4.0);
}

TEST(Equalizer, GainsClampedToRange) {
  Equalizer eq(48000.0, 2);
  eq.setBandGain(0, 50.0);   // over +10
  eq.setBandGain(1, -50.0);  // under -10
  EXPECT_LE(eq.gains()[0], 10.0);
  EXPECT_GE(eq.gains()[1], -10.0);
}

TEST(Delay, DelaysSignalByConfiguredSamples) {
  Delay d(48000.0, 1);
  d.setDelayMs(1.0);  // 48 samples at 48kHz
  std::vector<float> buf(96, 0.0f);
  buf[0] = 1.0f;  // impulse
  d.process(buf.data(), 96);
  // The impulse should now appear ~48 samples later.
  EXPECT_FLOAT_EQ(buf[0], 0.0f);
  EXPECT_NEAR(buf[48], 1.0f, 1e-6);
}

TEST(Limiter, CapsPeaksAboveThreshold) {
  Limiter lim(48000.0);
  lim.setThresholdDb(-6.0);  // ~0.5 linear
  lim.setAttackMs(0.1);
  auto buf = sine(1000.0, 48000.0, 4096, 0.95f);  // loud
  lim.process(buf.data(), 4096, 2);
  // After the envelope settles, peaks should be at/under threshold.
  float peak = 0;
  for (std::size_t i = buf.size() / 2; i < buf.size(); ++i) peak = std::max(peak, std::fabs(buf[i]));
  EXPECT_LE(peak, 0.55f);
}

TEST(GainControl, AttenuatesBy6dB) {
  GainControl g(-6.0);
  g.reset();
  std::vector<float> buf(1024, 0.5f);
  g.process(buf.data(), 512, 2);
  EXPECT_NEAR(buf[1000], 0.5f * 0.5011f, 0.01f);  // -6 dB ~= 0.501x
}

TEST(DspEngine, BypassLeavesPcmUnchanged) {
  nexus::core::EventBus bus;
  DspEngine dsp(&bus, 48000.0, 2);
  dsp.setBypass(true);
  std::vector<std::int16_t> pcm(1024, 1234);
  auto copy = pcm;
  dsp.processInt16(pcm.data(), 512, 2);
  EXPECT_EQ(pcm, copy);
}

TEST(DspEngine, FlatChainRoughlyPreservesSignal) {
  nexus::core::EventBus bus;
  DspEngine dsp(&bus, 48000.0, 2);
  dsp.setLimiter(false, -1.0);
  // Quiet sine so nothing limits/compresses.
  std::vector<std::int16_t> pcm(1024);
  for (std::size_t f = 0; f < 512; ++f) {
    auto s = static_cast<std::int16_t>(3000 * std::sin(2.0 * M_PI * 1000.0 * f / 48000.0));
    pcm[f * 2] = s;
    pcm[f * 2 + 1] = s;
  }
  double before = 0;
  for (auto x : pcm) before += double(x) * x;
  dsp.processInt16(pcm.data(), 512, 2);
  double after = 0;
  for (auto x : pcm) after += double(x) * x;
  // Energy should be within a reasonable band of the input (EQ flat, gains 0 dB).
  EXPECT_GT(after, before * 0.5);
  EXPECT_LT(after, before * 1.5);
}

TEST(DspEngine, OutputGainReducesEnergy) {
  nexus::core::EventBus bus;
  DspEngine dsp(&bus, 48000.0, 2);
  dsp.setLimiter(false, -1.0);
  dsp.setOutputGainDb(-20.0);
  std::vector<std::int16_t> pcm(2048);
  for (std::size_t f = 0; f < 1024; ++f) {
    auto s = static_cast<std::int16_t>(4000 * std::sin(2.0 * M_PI * 500.0 * f / 48000.0));
    pcm[f * 2] = s;
    pcm[f * 2 + 1] = s;
  }
  dsp.processInt16(pcm.data(), 1024, 2);
  // Tail (after gain smoothing settles) should be much quieter than the original amplitude.
  std::int16_t peak = 0;
  for (std::size_t i = pcm.size() - 200; i < pcm.size(); ++i)
    peak = std::max<std::int16_t>(peak, std::abs(pcm[i]));
  EXPECT_LT(peak, 2000);
}
