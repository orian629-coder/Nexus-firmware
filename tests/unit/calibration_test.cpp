#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <filesystem>
#include <memory>
#include <vector>

#include "calibration/AutoEq.h"
#include "calibration/AutoVolume.h"
#include "calibration/CalibrationManager.h"
#include "calibration/CalibrationProfiles.h"
#include "calibration/Fft.h"
#include "calibration/RoomMeasurement.h"
#include "config/ConfigManager.h"
#include "core/EventBus.h"

using namespace nexus;
using namespace nexus::calibration;
namespace fs = std::filesystem;

TEST(Fft, RecoversPeakAtSignalFrequency) {
  const std::size_t n = 1024;
  const double fs = 48000.0;
  const double freq = 3000.0;
  std::vector<double> sig(n);
  for (std::size_t i = 0; i < n; ++i)
    sig[i] = std::sin(2.0 * M_PI * freq * static_cast<double>(i) / fs);

  auto mag = magnitudeSpectrum(sig, n);
  // Find the peak bin.
  std::size_t peak = 0;
  for (std::size_t k = 1; k < mag.size(); ++k)
    if (mag[k] > mag[peak]) peak = k;
  const double peak_freq = static_cast<double>(peak) * fs / static_cast<double>(n);
  EXPECT_NEAR(peak_freq, freq, fs / n * 2);  // within a couple of bins
}

TEST(RoomMeasurement, DetectsBandEnergyOfTone) {
  RoomMeasurement room(48000.0, 8192);
  // 1 kHz tone → the band at 1 kHz should have the highest energy.
  std::vector<double> sig(48000);
  for (std::size_t i = 0; i < sig.size(); ++i)
    sig[i] = 0.5 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / 48000.0);
  auto bands = room.bandEnergyDb(sig);

  // Band index 15 == 1000 Hz (from dsp::kBandFreqs).
  int argmax = 0;
  for (int b = 1; b < dsp::kEqBands; ++b)
    if (bands[b] > bands[argmax]) argmax = b;
  EXPECT_NEAR(argmax, 15, 2);
}

TEST(AutoEq, BoostsQuietBandsCutsLoudBands) {
  AutoEq eq(1.0);
  std::array<double, dsp::kEqBands> measured{};
  measured.fill(-20.0);
  measured[5] = -30.0;  // quiet band → should be boosted
  measured[10] = -5.0;  // loud band → should be cut
  auto corr = eq.computeCorrection(measured);
  EXPECT_GT(corr[5], 0.0);
  EXPECT_LT(corr[10], 0.0);
}

TEST(AutoEq, FlatMeasurementScoresHigh) {
  std::array<double, dsp::kEqBands> flat{};
  flat.fill(-15.0);
  EXPECT_GT(AutoEq::flatnessScore(flat), 95.0);
}

TEST(AutoVolume, StepsGraduallyTowardTarget) {
  AutoVolume::Params p;
  p.max_step = 3;
  AutoVolume av(p);
  // A louder room than the current volume reflects → volume increases by at most max_step.
  int next = av.nextVolume(/*current*/ 30, /*ambient*/ -20.0);
  EXPECT_LE(std::abs(next - 30), 3);
  EXPECT_GT(next, 30);  // louder ambient → higher volume
}

TEST(AutoVolume, RespectsMaxClamp) {
  AutoVolume::Params p;
  p.max_volume = 80;
  p.max_step = 100;
  AutoVolume av(p);
  int next = av.nextVolume(75, /*very loud*/ 0.0);
  EXPECT_LE(next, 80);
}

TEST(CalibrationProfiles, SaveLoadRoundTrip) {
  auto dir = fs::temp_directory_path() / "nexus_cal_prof";
  fs::remove_all(dir);
  CalibrationProfiles profiles(dir.string());

  CalibrationProfile p;
  p.name = "room";
  p.score = 88.0;
  p.eq_gains[3] = 4.5;
  ASSERT_TRUE(profiles.save(p).ok());
  ASSERT_TRUE(profiles.has("room"));

  auto loaded = profiles.load("room");
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value().name, "room");
  EXPECT_NEAR(loaded.value().score, 88.0, 1e-6);
  EXPECT_NEAR(loaded.value().eq_gains[3], 4.5, 1e-6);
}

TEST(CalibrationManager, FullRunAppliesAndPersists) {
  auto dir = fs::temp_directory_path() / "nexus_cal_run";
  fs::remove_all(dir);
  fs::create_directories(dir);
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();
  auto profiles = std::make_shared<CalibrationProfiles>((dir / "cal").string());

  CalibrationManager cal(&bus, &config, profiles, 48000.0);
  cal.start();

  // Hooks: capture returns a tone (so analysis produces a non-flat correction); apply records the
  // gains; mute toggles a flag.
  std::array<double, dsp::kEqBands> applied{};
  bool applied_set = false;
  std::vector<bool> mute_calls;
  cal.setHooks(
      [](std::size_t frames) {
        std::vector<std::int16_t> pcm(frames);
        for (std::size_t i = 0; i < frames; ++i)
          pcm[i] = static_cast<std::int16_t>(
              8000 * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / 48000.0));
        return pcm;
      },
      [&](const std::array<double, dsp::kEqBands>& g) {
        applied = g;
        applied_set = true;
      },
      [&](bool m) { mute_calls.push_back(m); });

  int completed = 0;
  bus.subscribe(core::EventType::CalibrationCompleted, [&](const core::Event&) { ++completed; });

  auto res = cal.runCalibration("room");
  bus.drain();

  ASSERT_TRUE(res.ok());
  EXPECT_EQ(cal.calState(), CalState::Completed);
  EXPECT_TRUE(applied_set);
  EXPECT_EQ(completed, 1);
  // Mute during measurement, unmute at the end.
  ASSERT_GE(mute_calls.size(), 2u);
  EXPECT_TRUE(mute_calls.front());
  EXPECT_FALSE(mute_calls.back());
  // Config records the profile; profile persisted.
  EXPECT_EQ(config.get().audio.eq_profile, "room");
  EXPECT_TRUE(profiles->has("room"));
}

TEST(CalibrationManager, FailsGracefullyOnEmptyCapture) {
  auto dir = fs::temp_directory_path() / "nexus_cal_fail";
  fs::remove_all(dir);
  fs::create_directories(dir);
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();
  auto profiles = std::make_shared<CalibrationProfiles>((dir / "cal").string());

  CalibrationManager cal(&bus, &config, profiles, 48000.0);
  cal.start();
  bool unmuted_at_end = false;
  cal.setHooks([](std::size_t) { return std::vector<std::int16_t>{}; },  // empty capture
               [](const std::array<double, dsp::kEqBands>&) {},
               [&](bool m) { unmuted_at_end = !m; });

  int failed = 0;
  bus.subscribe(core::EventType::CalibrationFailed, [&](const core::Event&) { ++failed; });
  auto res = cal.runCalibration("room");
  bus.drain();

  EXPECT_FALSE(res.ok());
  EXPECT_EQ(cal.calState(), CalState::Failed);
  EXPECT_EQ(failed, 1);
  EXPECT_TRUE(unmuted_at_end);  // playback restored even on failure
}
