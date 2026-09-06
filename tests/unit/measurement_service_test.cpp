// Speaker-side measurement (F3): play a chirp out of this speaker while recording its microphones,
// and turn each channel into a distance.
//
// The hardware is injected, so these tests build a synthetic capture with a KNOWN delay per
// microphone and require the service to recover it. That is the only way to check the
// de-interleaving and per-mic bookkeeping without two real microphones.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "measure/MeasurementService.h"

using namespace nexus::measure;

namespace {

constexpr double kFs = 48000.0;

// Build an interleaved capture where each microphone hears the stimulus after its own delay.
// `delays` is per channel, in samples.
MeasurementService::Capture makeCapture(const std::vector<double>& stimulus,
                                        const std::vector<std::size_t>& delays,
                                        double gain = 0.5, double noise = 0.0) {
  const int channels = static_cast<int>(delays.size());
  std::size_t max_delay = 0;
  for (std::size_t d : delays) max_delay = std::max(max_delay, d);
  const std::size_t frames = stimulus.size() + max_delay + 1000;

  MeasurementService::Capture cap;
  cap.channels = channels;
  cap.pcm.assign(frames * static_cast<std::size_t>(channels), 0);

  std::uint32_t seed = 999;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (static_cast<double>(seed >> 8) / 8388608.0) - 1.0;
  };

  for (int c = 0; c < channels; ++c) {
    for (std::size_t i = 0; i < frames; ++i) {
      double v = noise > 0.0 ? rnd() * noise : 0.0;
      const std::size_t d = delays[static_cast<std::size_t>(c)];
      if (i >= d && i - d < stimulus.size()) v += stimulus[i - d] * gain;
      cap.pcm[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] =
          static_cast<std::int16_t>(std::max(-1.0, std::min(1.0, v)) * 32767.0);
    }
  }
  return cap;
}

MeasurementRequest baseRequest() {
  MeasurementRequest r;
  r.duration_s = 0.2;  // short, so the tests stay fast
  r.sample_rate = kFs;
  return r;
}

// Samples of delay for a given distance, given no latency budget.
std::size_t samplesFor(double metres) {
  return static_cast<std::size_t>(metres / kSpeedOfSoundMps * kFs);
}

}  // namespace

TEST(MeasurementService, RecoversTwoMicDistancesFromOneEmission) {
  // The whole point of capturing both mics at once: one chirp, two distances.
  const std::size_t d1 = samplesFor(2.0);
  const std::size_t d2 = samplesFor(3.5);

  MeasurementService svc([&](const std::vector<double>& stim) {
    return makeCapture(stim, {d1, d2});
  });

  const auto r = svc.measure(baseRequest());
  ASSERT_TRUE(r.ok) << r.message;
  ASSERT_EQ(r.mics.size(), 2u);

  EXPECT_TRUE(r.mics[0].valid);
  EXPECT_TRUE(r.mics[1].valid);
  EXPECT_NEAR(r.mics[0].distance_m, 2.0, 0.05);
  EXPECT_NEAR(r.mics[1].distance_m, 3.5, 0.05);
  EXPECT_EQ(r.mics[0].mic_index, 0);
  EXPECT_EQ(r.mics[1].mic_index, 1);
}

// De-interleaving is easy to get subtly wrong (swapped channels look plausible), so this asserts
// the mics are not confused with each other.
TEST(MeasurementService, DoesNotSwapMicrophoneChannels) {
  MeasurementService svc([&](const std::vector<double>& stim) {
    return makeCapture(stim, {samplesFor(1.0), samplesFor(6.0)});
  });

  const auto r = svc.measure(baseRequest());
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.mics.size(), 2u);
  EXPECT_LT(r.mics[0].distance_m, r.mics[1].distance_m)
      << "mic 0 was the near one; a swap would invert this";
}

// The latency budget must be removed from EVERY microphone, not just the first.
TEST(MeasurementService, AppliesTheLatencyBudgetToEveryMic) {
  const double budget_ms = 40.0;
  const std::size_t budget_samples = static_cast<std::size_t>(budget_ms * kFs / 1000.0);

  MeasurementService svc([&](const std::vector<double>& stim) {
    return makeCapture(stim, {budget_samples + samplesFor(1.5),
                              budget_samples + samplesFor(4.0)});
  });

  auto req = baseRequest();
  req.budget.hardware_rtl_ms = 25.0;
  req.budget.network_rtl_ms = 15.0;  // 40 ms total

  const auto r = svc.measure(req);
  ASSERT_TRUE(r.ok);
  EXPECT_NEAR(r.mics[0].distance_m, 1.5, 0.05);
  EXPECT_NEAR(r.mics[1].distance_m, 4.0, 0.05);
}

// One deaf microphone must not discard the other's reading — a real install can easily have a mic
// that cannot hear a particular speaker.
TEST(MeasurementService, OneSilentMicDoesNotInvalidateTheOther) {
  MeasurementService svc([&](const std::vector<double>& stim) {
    auto cap = makeCapture(stim, {samplesFor(2.0), samplesFor(2.0)});
    // Silence channel 1 entirely.
    for (std::size_t i = 1; i < cap.pcm.size(); i += 2) cap.pcm[i] = 0;
    return cap;
  });

  const auto r = svc.measure(baseRequest());
  EXPECT_TRUE(r.ok) << "partial success is still success";
  ASSERT_EQ(r.mics.size(), 2u);
  EXPECT_TRUE(r.mics[0].valid);
  EXPECT_FALSE(r.mics[1].valid);
  EXPECT_FALSE(r.mics[1].note.empty()) << "and it must say why";
}

TEST(MeasurementService, AllMicsSilentIsAFailureWithAReason) {
  MeasurementService svc([&](const std::vector<double>& stim) {
    MeasurementService::Capture cap;
    cap.channels = 2;
    cap.pcm.assign((stim.size() + 1000) * 2, 0);
    return cap;
  });

  const auto r = svc.measure(baseRequest());
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.message.empty());
}

TEST(MeasurementService, SelectsOnlyTheRequestedMics) {
  MeasurementService svc([&](const std::vector<double>& stim) {
    return makeCapture(stim, {samplesFor(1.0), samplesFor(2.0), samplesFor(3.0)});
  });

  auto req = baseRequest();
  req.mic_indices = {2};

  const auto r = svc.measure(req);
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.mics.size(), 1u);
  EXPECT_EQ(r.mics[0].mic_index, 2);
  EXPECT_NEAR(r.mics[0].distance_m, 3.0, 0.05);
}

TEST(MeasurementService, OutOfRangeMicIsReportedNotCrashed) {
  MeasurementService svc([&](const std::vector<double>& stim) {
    return makeCapture(stim, {samplesFor(1.0)});
  });

  auto req = baseRequest();
  req.mic_indices = {0, 5};

  const auto r = svc.measure(req);
  ASSERT_EQ(r.mics.size(), 2u);
  EXPECT_TRUE(r.mics[0].valid);
  EXPECT_FALSE(r.mics[1].valid);
  EXPECT_NE(r.mics[1].note.find("no such microphone"), std::string::npos);
}

TEST(MeasurementService, SurvivesNoisyCapture) {
  MeasurementService svc([&](const std::vector<double>& stim) {
    return makeCapture(stim, {samplesFor(2.5)}, /*gain=*/0.15, /*noise=*/0.05);
  });

  const auto r = svc.measure(baseRequest());
  ASSERT_TRUE(r.ok) << r.message;
  EXPECT_NEAR(r.mics[0].distance_m, 2.5, 0.15);
  EXPECT_GT(r.mics[0].confidence, 1.5) << "a real arrival should beat the noise floor";
}

// ── failure modes that must not be silent ──

TEST(MeasurementService, NoCaptureBackendIsReported) {
  MeasurementService svc(nullptr);
  const auto r = svc.measure(baseRequest());
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.message.find("no capture backend"), std::string::npos);
}

TEST(MeasurementService, CaptureShorterThanStimulusIsRejected) {
  MeasurementService svc([](const std::vector<double>&) {
    MeasurementService::Capture cap;
    cap.channels = 1;
    cap.pcm.assign(100, 0);  // far shorter than the chirp
    return cap;
  });

  const auto r = svc.measure(baseRequest());
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.message.find("shorter"), std::string::npos);
}

TEST(MeasurementService, InvalidParametersAreRejected) {
  MeasurementService svc([](const std::vector<double>&) {
    return MeasurementService::Capture{};
  });

  auto bad_rate = baseRequest();
  bad_rate.sample_rate = 0.0;
  EXPECT_FALSE(svc.measure(bad_rate).ok);

  auto bad_dur = baseRequest();
  bad_dur.duration_s = 0.0;
  EXPECT_FALSE(svc.measure(bad_dur).ok);
}

// The stimulus handed to the hardware must be the windowed chirp, not raw samples — an unwindowed
// sweep's start click competes with the real arrival.
TEST(MeasurementService, EmitsAWindowedChirp) {
  std::vector<double> seen;
  MeasurementService svc([&](const std::vector<double>& stim) {
    seen = stim;
    return makeCapture(stim, {samplesFor(1.0)});
  });

  auto req = baseRequest();
  req.duration_s = 0.1;
  svc.measure(req);

  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.size(), static_cast<std::size_t>(0.1 * kFs));
  EXPECT_NEAR(seen.front(), 0.0, 1e-9);
  EXPECT_NEAR(seen.back(), 0.0, 1e-9);
}
