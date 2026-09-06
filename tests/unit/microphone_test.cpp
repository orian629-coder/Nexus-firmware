#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <memory>

#include "core/EventBus.h"
#include "microphone/MicrophoneManager.h"
#include "microphone/NoiseMonitor.h"
#include "microphone/SplMeter.h"

using namespace nexus::microphone;
using nexus::core::Event;
using nexus::core::EventBus;
using nexus::core::EventType;

TEST(SplMeter, SilenceIsVeryLow) {
  std::vector<std::int16_t> silence(1000, 0);
  EXPECT_LT(SplMeter::rmsDbfs(silence), -100.0);
}

TEST(SplMeter, LouderSignalHasHigherDbfs) {
  std::vector<std::int16_t> quiet(1000, 100);
  std::vector<std::int16_t> loud(1000, 10000);
  EXPECT_GT(SplMeter::rmsDbfs(loud), SplMeter::rmsDbfs(quiet));
}

TEST(SplMeter, SplAddsReferenceOffset) {
  SplMeter m(94.0);
  std::vector<std::int16_t> pcm(1000, 3000);
  EXPECT_NEAR(m.splDb(pcm), SplMeter::rmsDbfs(pcm) + 94.0, 1e-6);
}

TEST(NoiseMonitor, ConvergesTowardLevel) {
  NoiseMonitor nm(0.5);
  std::vector<std::int16_t> pcm(1000, 2000);
  double a1 = nm.update(pcm);
  double a2 = nm.update(pcm);
  EXPECT_LE(std::fabs(a2 - SplMeter::rmsDbfs(pcm)), std::fabs(a1 - SplMeter::rmsDbfs(pcm)) + 1e-9);
}

namespace {
struct Rig {
  EventBus bus;
  StubMicrophoneHal* hal;
  std::unique_ptr<MicrophoneManager> mic;
  Rig() {
    auto h = std::make_unique<StubMicrophoneHal>();
    hal = h.get();
    mic = std::make_unique<MicrophoneManager>(&bus, std::move(h));
  }
};
}  // namespace

TEST(MicrophoneManager, MeasuresSplFromCapture) {
  Rig r;
  r.mic->start();
  auto spl = r.mic->measureSpl(4800);
  ASSERT_TRUE(spl.ok());
  EXPECT_GT(spl.value(), 0.0);  // ambient + 94 dB ref > 0
}

TEST(MicrophoneManager, SelfTestPassesWithSignal) {
  Rig r;
  r.mic->start();
  EXPECT_TRUE(r.mic->selfTest().ok());
}

TEST(MicrophoneManager, SelfTestFailsAndAlertsOnCaptureError) {
  Rig r;
  r.mic->start();
  r.hal->fail = true;

  std::atomic<int> mic_fail{0};
  r.bus.subscribe(EventType::MicFailure, [&](const Event&) { ++mic_fail; });
  EXPECT_FALSE(r.mic->selfTest().ok());
  r.bus.drain();
  EXPECT_EQ(mic_fail.load(), 1);
}

TEST(MicrophoneManager, AmbientTracksLouderEnvironment) {
  Rig r;
  r.mic->start();
  r.hal->amplitude = 200.0;
  double quiet = r.mic->updateAmbient(4800).value_or(0);
  r.hal->amplitude = 5000.0;
  for (int i = 0; i < 10; ++i) r.mic->updateAmbient(4800);
  double loud = r.mic->ambientDbfs();
  EXPECT_GT(loud, quiet);
}
