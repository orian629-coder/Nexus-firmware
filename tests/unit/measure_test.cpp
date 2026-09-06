// Acoustic distance measurement (F1 + F2).
//
// The load-bearing test here is ReproducesThePrototypeReferenceLag: it runs this C++ implementation
// over the SAME recording the Python prototype was validated with, and requires the same answer.
// Without that, "the correlation looks right" is an opinion — a measurement module that is
// self-consistently wrong passes every synthetic test.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "measure/ArrivalDetector.h"
#include "measure/Chirp.h"
#include "measure/Distance.h"

using namespace nexus::measure;

namespace {

constexpr double kFs = 44100.0;

// Minimal reader for the float32 ("format 3") WAVs the prototype wrote. Python's own `wave` module
// refuses these, which is why the reference had to be recomputed with a manual parser too.
bool readFloatWav(const std::string& path, std::vector<double>& out, double& sample_rate) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> buf(static_cast<std::size_t>(size));
  const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  if (got != buf.size() || buf.size() < 44) return false;

  auto u32 = [&](std::size_t i) {
    return static_cast<std::uint32_t>(buf[i]) | (static_cast<std::uint32_t>(buf[i + 1]) << 8) |
           (static_cast<std::uint32_t>(buf[i + 2]) << 16) |
           (static_cast<std::uint32_t>(buf[i + 3]) << 24);
  };

  std::size_t fmt = std::string::npos, data = std::string::npos;
  for (std::size_t i = 12; i + 8 <= buf.size(); ++i) {
    if (std::memcmp(&buf[i], "fmt ", 4) == 0) fmt = i;
    if (std::memcmp(&buf[i], "data", 4) == 0) { data = i; break; }
  }
  if (fmt == std::string::npos || data == std::string::npos) return false;

  sample_rate = static_cast<double>(u32(fmt + 12));
  const std::uint32_t bytes = u32(data + 4);
  const std::size_t n = bytes / 4;
  out.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    float v;
    std::memcpy(&v, &buf[data + 8 + i * 4], 4);
    out[i] = static_cast<double>(v);
  }
  return true;
}

std::string referenceDir() { return std::string(NEXUS_MEASURE_REF_DIR); }

}  // namespace

// ── the reference check ──

// The prototype's own RTL recording. Verified independently with numpy: the correlation peaks at
// lag 2075 samples = 47.0522 ms, which is exactly the 47.052 the prototype's UI carries as its
// hardware RTL. Any change that moves this number has broken the measurement.
TEST(Measure, ReproducesThePrototypeReferenceLag) {
  std::vector<double> guide, rec;
  double fs_g = 0.0, fs_r = 0.0;
  const std::string dir = referenceDir();

  if (!readFloatWav(dir + "/latency_guide.wav", guide, fs_g) ||
      !readFloatWav(dir + "/latency_rec.wav", rec, fs_r)) {
    GTEST_SKIP() << "reference WAVs not present at " << dir;
  }

  ASSERT_DOUBLE_EQ(fs_g, kFs);
  ASSERT_DOUBLE_EQ(fs_r, kFs);
  ASSERT_EQ(guide.size(), 44100u);
  ASSERT_EQ(rec.size(), 44100u);

  // The recording is the same length as the stimulus, so correlation has to be run over a padded
  // recording — the real capture in the field is longer than the stimulus by design.
  std::vector<double> padded = rec;
  padded.resize(rec.size() * 2, 0.0);

  // Bounded search: the answer is a loopback latency, so anything past ~200 ms is not a candidate.
  // Unbounded correlation over a 1 s stimulus is ~2e9 multiply-adds and takes ~15 s, which is too
  // slow to keep in the suite; the bound is physical, not a shortcut.
  const long max_lag = static_cast<long>(0.2 * kFs);
  const Arrival a = findArrival(padded, guide, kFs, max_lag);

  ASSERT_TRUE(a.found);
  EXPECT_EQ(a.lag_samples, 2075) << "must match the numpy reference exactly";
  EXPECT_NEAR(a.lag_ms, 47.0522, 0.001);
}

// With that lag used as the hardware RTL, the mic-at-the-speaker case must read ~zero distance.
TEST(Measure, ReferenceRecordingYieldsZeroDistance) {
  LatencyBudget budget;
  budget.hardware_rtl_ms = 47.052;
  budget.network_rtl_ms = 0.0;  // local loopback, nothing streamed

  const auto d = toDistance(47.0522, budget);
  EXPECT_TRUE(d.valid);
  EXPECT_NEAR(d.distance_m, 0.0, 0.01);
}

// ── synthetic round trips ──

// A known delay inserted into a synthetic recording must come back as that delay.
TEST(Measure, RecoversAKnownDelay) {
  const auto stim = logChirp(0.2, kFs);
  const std::size_t delay = 1000;

  std::vector<double> rec(stim.size() + delay * 3, 0.0);
  for (std::size_t i = 0; i < stim.size(); ++i) rec[delay + i] = stim[i] * 0.3;

  const Arrival a = findArrival(rec, stim, kFs);
  ASSERT_TRUE(a.found);
  EXPECT_EQ(a.lag_samples, static_cast<long>(delay));
}

// Attenuation must not move the peak: normalization is what makes a quiet, distant speaker measure
// the same distance as a loud near one.
TEST(Measure, LevelDoesNotChangeTheMeasuredLag) {
  const auto stim = logChirp(0.2, kFs);
  const std::size_t delay = 700;

  auto build = [&](double gain) {
    std::vector<double> rec(stim.size() + delay * 3, 0.0);
    for (std::size_t i = 0; i < stim.size(); ++i) rec[delay + i] = stim[i] * gain;
    return rec;
  };

  const Arrival loud = findArrival(build(0.9), stim, kFs);
  const Arrival quiet = findArrival(build(0.02), stim, kFs);
  EXPECT_EQ(loud.lag_samples, quiet.lag_samples);
  EXPECT_NEAR(loud.peak, quiet.peak, 1e-9) << "normalized peak must be level-independent";
}

// A DC offset is a real converter artifact and must not bias the result.
TEST(Measure, DcOffsetDoesNotBiasTheResult) {
  const auto stim = logChirp(0.2, kFs);
  const std::size_t delay = 512;
  std::vector<double> rec(stim.size() + delay * 3, 0.25);  // whole capture sits on +0.25
  for (std::size_t i = 0; i < stim.size(); ++i) rec[delay + i] += stim[i] * 0.4;

  const Arrival a = findArrival(rec, stim, kFs);
  ASSERT_TRUE(a.found);
  EXPECT_EQ(a.lag_samples, static_cast<long>(delay));
}

TEST(Measure, SurvivesNoise) {
  const auto stim = logChirp(0.3, kFs);
  const std::size_t delay = 1500;
  std::vector<double> rec(stim.size() + delay * 2, 0.0);

  // Deterministic pseudo-noise at roughly the same level as the attenuated signal.
  std::uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (static_cast<double>(seed >> 8) / 8388608.0) - 1.0;
  };
  for (auto& v : rec) v = rnd() * 0.05;
  for (std::size_t i = 0; i < stim.size(); ++i) rec[delay + i] += stim[i] * 0.05;

  const Arrival a = findArrival(rec, stim, kFs);
  ASSERT_TRUE(a.found);
  EXPECT_NEAR(a.lag_samples, static_cast<long>(delay), 2);
  EXPECT_GT(a.confidence(), 1.5) << "a real detection should clearly beat the noise floor";
}

TEST(Measure, SilenceIsNotAFalseDetection) {
  const auto stim = logChirp(0.2, kFs);
  const std::vector<double> silence(stim.size() * 3, 0.0);

  const Arrival a = findArrival(silence, stim, kFs);
  EXPECT_FALSE(a.found) << "digital silence must not produce a distance";
}

TEST(Measure, DegenerateInputsAreRejected) {
  const auto stim = logChirp(0.1, kFs);
  EXPECT_FALSE(findArrival({}, stim, kFs).found);
  EXPECT_FALSE(findArrival(stim, {}, kFs).found);
  EXPECT_FALSE(findArrival(stim, stim, 0.0).found);
  // A recording shorter than the stimulus cannot contain it.
  EXPECT_FALSE(findArrival(std::vector<double>(10, 0.5), stim, kFs).found);
}

// ── stimulus shape ──

TEST(Chirp, IsWindowedAtBothEnds) {
  const auto c = logChirp(0.1, kFs);
  ASSERT_FALSE(c.empty());
  // The Hanning window exists to kill the start/end click that would otherwise correlate as a
  // competing arrival.
  EXPECT_NEAR(c.front(), 0.0, 1e-9);
  EXPECT_NEAR(c.back(), 0.0, 1e-9);
  double peak = 0.0;
  for (double v : c) peak = std::max(peak, std::fabs(v));
  EXPECT_GT(peak, 0.1) << "and it must still carry energy in the middle";
}

TEST(Chirp, SweepsUpwardInFrequency) {
  const auto c = logChirp(0.5, kFs, 500.0, 8000.0);
  // Count zero crossings in the first and last tenth: a rising sweep has many more at the end.
  auto crossings = [&](std::size_t from, std::size_t to) {
    int n = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
      if ((c[i - 1] < 0.0) != (c[i] < 0.0)) ++n;
    }
    return n;
  };
  const std::size_t tenth = c.size() / 10;
  EXPECT_GT(crossings(c.size() - 2 * tenth, c.size() - tenth), crossings(tenth, 2 * tenth));
}

TEST(Impulse, IsCentredAndSingleSample) {
  const auto p = impulse(0.1, kFs);
  ASSERT_EQ(p.size(), 4410u);
  int nonzero = 0;
  for (double v : p) {
    if (v != 0.0) ++nonzero;
  }
  EXPECT_EQ(nonzero, 1);
  EXPECT_GT(p[p.size() / 2], 0.0) << "centred, so the peak has room on both sides";
}

// ── the latency budget ──

// The bug this guards against: the prototype subtracts hardware AND network RTL. Forgetting the
// network term adds the whole streaming latency to every distance — at the ~45 ms this system
// measures, that is about 15 metres.
TEST(Distance, SubtractsBothHardwareAndNetworkLatency) {
  LatencyBudget budget;
  budget.hardware_rtl_ms = 47.0;
  budget.network_rtl_ms = 45.0;
  EXPECT_DOUBLE_EQ(budget.totalMs(), 92.0);

  // 2 m of air ≈ 5.83 ms.
  const double lag = 92.0 + 5.83;
  const auto d = toDistance(lag, budget);
  ASSERT_TRUE(d.valid);
  EXPECT_NEAR(d.distance_m, 2.0, 0.01);

  // Same lag, network term forgotten: the answer is wrong by ~15 m, not slightly off.
  LatencyBudget forgot;
  forgot.hardware_rtl_ms = 47.0;
  const auto bad = toDistance(lag, forgot);
  EXPECT_GT(bad.distance_m, 15.0);
}

TEST(Distance, MicAtTheSpeakerReadsZeroNotNegative) {
  LatencyBudget b;
  b.hardware_rtl_ms = 47.0;
  // 0.2 ms early — inside the tolerance, i.e. RTL rounding, not a real negative distance.
  const auto d = toDistance(46.8, b);
  EXPECT_TRUE(d.valid);
  EXPECT_DOUBLE_EQ(d.distance_m, 0.0);
}

// A large negative time means the budget is wrong. Clamping it would hide a systematic error behind
// a plausible number, so it is refused instead.
TEST(Distance, ImpossiblyShortLagIsRefusedNotClamped) {
  LatencyBudget b;
  b.hardware_rtl_ms = 47.0;
  b.network_rtl_ms = 45.0;

  const auto d = toDistance(50.0, b);  // 42 ms short of the budget
  EXPECT_FALSE(d.valid);
  EXPECT_NE(d.note.find("latency budget"), std::string::npos);
}

TEST(Distance, SpeedOfSoundTracksTemperature) {
  EXPECT_NEAR(speedOfSoundAt(20.0), 343.4, 0.2);
  EXPECT_GT(speedOfSoundAt(30.0), speedOfSoundAt(20.0));

  // A 10 °C error is ~2 %: at 5 m that is ~10 cm, which matters for speaker placement.
  LatencyBudget b;
  const double lag = 14.577;  // ~5 m at 343 m/s
  const auto cold = toDistance(lag, b, speedOfSoundAt(10.0));
  const auto hot = toDistance(lag, b, speedOfSoundAt(30.0));
  EXPECT_GT(hot.distance_m - cold.distance_m, 0.05);
}
