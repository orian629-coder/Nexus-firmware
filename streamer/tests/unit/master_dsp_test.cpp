// Master DSP chain: the streamer's global processing stage.
//
// The requirements under test are the ones stated for the feature: bypass must disable processing
// WITHOUT interrupting the stream, EQ must actually change the signal, a partial config update must
// not clobber unrelated settings, and the whole thing must survive a save/load round trip.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "dsp/MasterDsp.h"

using nexus::streamer::dsp::MasterDsp;
using nexus::streamer::dsp::MasterDspConfig;

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;

// A stereo sine at `freq`, as interleaved int16.
std::vector<std::int16_t> tone(double freq, std::size_t frames, double amplitude = 8000.0) {
  std::vector<std::int16_t> pcm(frames * kChannels);
  for (std::size_t f = 0; f < frames; ++f) {
    const double s = std::sin(2.0 * M_PI * freq * static_cast<double>(f) / kRate) * amplitude;
    pcm[f * kChannels] = pcm[f * kChannels + 1] = static_cast<std::int16_t>(s);
  }
  return pcm;
}

double peak(const std::vector<std::int16_t>& pcm) {
  double m = 0.0;
  for (std::int16_t s : pcm) m = std::max(m, std::fabs(static_cast<double>(s)));
  return m;
}

// RMS is the honest measure of "did the level change" for a filtered sine, where a single peak can
// land between samples.
double rms(const std::vector<std::int16_t>& pcm) {
  if (pcm.empty()) return 0.0;
  double sum = 0.0;
  for (std::int16_t s : pcm) sum += static_cast<double>(s) * static_cast<double>(s);
  return std::sqrt(sum / static_cast<double>(pcm.size()));
}

}  // namespace

// ── bypass ──

// The stated requirement: "Bypass must disable the EQ safely without stopping the audio stream."
// Both halves matter — samples must come out unchanged AND they must still come out.
TEST(MasterDsp, BypassPassesAudioThroughByteForByte) {
  MasterDsp dsp(kRate, kChannels);
  MasterDspConfig c;
  c.bypass = true;
  c.input_gain_db = 6.0;         // deliberately non-neutral settings that MUST be ignored
  c.master_volume_db = -6.0;
  c.eq_gains_db[10] = 9.0;
  dsp.setConfig(c);

  const auto original = tone(1000.0, 480);
  auto buf = original;
  dsp.process(buf.data(), 480, kChannels);

  EXPECT_EQ(buf, original) << "bypass must not alter a single sample";
}

TEST(MasterDsp, TogglingBypassKeepsStreamFlowing) {
  MasterDsp dsp(kRate, kChannels);
  // Alternate bypass on/off across consecutive blocks, as a UI toggle would mid-playback. Every
  // block must still produce output — the failure this guards against is a chain that drops or
  // zeroes audio while being reconfigured.
  for (int block = 0; block < 20; ++block) {
    MasterDspConfig c;
    c.bypass = (block % 2 == 0);
    c.eq_gains_db[5] = 6.0;
    dsp.setConfig(c);

    auto buf = tone(1000.0, 480);
    dsp.process(buf.data(), 480, kChannels);

    EXPECT_EQ(buf.size(), static_cast<std::size_t>(480 * kChannels));
    EXPECT_GT(rms(buf), 0.0) << "block " << block << " came out silent";
  }
}

// ── EQ actually does something ──

TEST(MasterDsp, EqBoostRaisesLevelAndCutLowersIt) {
  const auto input = tone(1000.0, 4800);

  // Index 15 is 1000 Hz in kBandFreqs — the tone's own frequency. Using a band that does NOT match
  // the tone measures only the filter's skirt and can move the level the wrong way.
  ASSERT_DOUBLE_EQ(nexus::dsp::kBandFreqs[15], 1000.0);

  auto measure = [&](double gain_db) {
    MasterDsp dsp(kRate, kChannels);
    MasterDspConfig c;
    c.limiter_enabled = false;  // isolate the EQ: a limiter would mask a boost
    c.eq_gains_db[15] = gain_db;
    dsp.setConfig(c);
    auto buf = input;
    dsp.process(buf.data(), input.size() / kChannels, kChannels);
    return rms(buf);
  };

  const double flat = measure(0.0);
  const double boosted = measure(9.0);
  const double cut = measure(-9.0);

  EXPECT_GT(boosted, flat * 1.05) << "a +9 dB boost at the tone's frequency did nothing";
  EXPECT_LT(cut, flat * 0.95) << "a -9 dB cut at the tone's frequency did nothing";
}

TEST(MasterDsp, MasterVolumeAttenuates) {
  MasterDsp dsp(kRate, kChannels);
  MasterDspConfig c;
  c.master_volume_db = -20.0;
  c.limiter_enabled = false;
  dsp.setConfig(c);

  // Long enough for the per-sample gain ramp (which exists to prevent clicks) to settle.
  auto buf = tone(1000.0, 48000);
  const double before = rms(buf);
  dsp.process(buf.data(), 48000, kChannels);
  EXPECT_LT(rms(buf), before * 0.5) << "-20 dB should be clearly quieter";
}

// ── limiter ──

TEST(MasterDsp, LimiterHoldsBackAHotSignal) {
  MasterDsp dsp(kRate, kChannels);
  MasterDspConfig c;
  c.limiter_enabled = true;
  c.limiter_threshold_db = -6.0;
  c.input_gain_db = 12.0;  // drive it hard on purpose
  dsp.setConfig(c);

  auto buf = tone(1000.0, 48000, 20000.0);
  dsp.process(buf.data(), 48000, kChannels);

  // Must not reach full scale: the point of a master limiter feeding N speakers.
  EXPECT_LT(peak(buf), 32767.0);
}

// ── config handling ──

TEST(MasterDsp, EqGainsAreClampedToTheSpeakersRange) {
  MasterDsp dsp(kRate, kChannels);
  MasterDspConfig c;
  c.eq_gains_db[0] = 99.0;
  c.eq_gains_db[1] = -99.0;
  dsp.setConfig(c);

  // Reported config must match what the engine will really do, not what was asked for — the
  // speaker's Equalizer clamps to +/-10 dB regardless.
  const auto applied = dsp.config();
  EXPECT_DOUBLE_EQ(applied.eq_gains_db[0], 10.0);
  EXPECT_DOUBLE_EQ(applied.eq_gains_db[1], -10.0);
}

TEST(MasterDspConfigJson, RoundTripsThroughJson) {
  MasterDspConfig c;
  c.bypass = true;
  c.input_gain_db = 3.5;
  c.master_volume_db = -2.5;
  c.limiter_enabled = false;
  c.limiter_threshold_db = -3.0;
  c.eq_gains_db[7] = 4.0;
  c.eq_gains_db[31] = -4.0;

  const auto back = MasterDspConfig::fromJson(c.toJson());

  EXPECT_EQ(back.bypass, c.bypass);
  EXPECT_DOUBLE_EQ(back.input_gain_db, c.input_gain_db);
  EXPECT_DOUBLE_EQ(back.master_volume_db, c.master_volume_db);
  EXPECT_EQ(back.limiter_enabled, c.limiter_enabled);
  EXPECT_DOUBLE_EQ(back.limiter_threshold_db, c.limiter_threshold_db);
  EXPECT_DOUBLE_EQ(back.eq_gains_db[7], 4.0);
  EXPECT_DOUBLE_EQ(back.eq_gains_db[31], -4.0);
}

// A hand-edited or older config must still boot with a usable chain rather than throwing.
TEST(MasterDspConfigJson, MalformedJsonFallsBackToDefaults) {
  EXPECT_NO_THROW({
    auto c = MasterDspConfig::fromJson(nlohmann::json::array({1, 2, 3}));
    EXPECT_FALSE(c.bypass);
    EXPECT_DOUBLE_EQ(c.eq_gains_db[0], 0.0);
  });

  // Wrong-length EQ array: fill what is there, leave the rest flat.
  auto partial = MasterDspConfig::fromJson({{"eq_gains_db", {5.0, 6.0}}});
  EXPECT_DOUBLE_EQ(partial.eq_gains_db[0], 5.0);
  EXPECT_DOUBLE_EQ(partial.eq_gains_db[1], 6.0);
  EXPECT_DOUBLE_EQ(partial.eq_gains_db[2], 0.0);

  // A non-numeric entry must not corrupt the band or throw.
  auto bad = MasterDspConfig::fromJson({{"eq_gains_db", {"loud", 6.0}}});
  EXPECT_DOUBLE_EQ(bad.eq_gains_db[0], 0.0);
  EXPECT_DOUBLE_EQ(bad.eq_gains_db[1], 6.0);
}

// The merge semantics the API relies on: PATCHing one field must leave the others intact. This is
// what lets the UI move a single slider without resending all 32 bands.
TEST(MasterDspConfigJson, PartialUpdatePreservesOtherFields) {
  MasterDspConfig c;
  c.eq_gains_db[3] = 7.0;
  c.master_volume_db = -4.0;

  nlohmann::json merged = c.toJson();
  merged.update(nlohmann::json{{"bypass", true}});
  const auto after = MasterDspConfig::fromJson(merged);

  EXPECT_TRUE(after.bypass);
  EXPECT_DOUBLE_EQ(after.eq_gains_db[3], 7.0) << "an unrelated EQ band was reset";
  EXPECT_DOUBLE_EQ(after.master_volume_db, -4.0) << "an unrelated gain was reset";
}
