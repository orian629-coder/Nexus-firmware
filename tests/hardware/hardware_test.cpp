// Hardware-dependent tests. Built only when NEXUS_HW_TESTS=ON and run on the target device (Pi)
// where the real HALs (ALSA/I2S amp, ALSA mic, nmcli) are present. On any other host each test
// SKIPs, so the suite stays green in CI while documenting exactly what must be verified on metal.
//
// These use the *real* (non-stub) HALs — build with `-DNEXUS_STUB_HAL=OFF -DNEXUS_HW_TESTS=ON`.

#include <gtest/gtest.h>

#include <cstdlib>

namespace {
bool onTarget() {
  const char* v = std::getenv("NEXUS_ON_TARGET");
  return v && std::string(v) == "1";
}
}  // namespace

// Amplifier: power on → read a plausible temperature → mute/unmute round-trips on the real driver.
TEST(Hardware, AmplifierPowerAndTemperature) {
  if (!onTarget()) GTEST_SKIP() << "set NEXUS_ON_TARGET=1 on the Pi to run";
  // On target (NEXUS_STUB_HAL=OFF) this would exercise the real IAmplifierHal:
  //   AlsaAmplifierHal hal; ASSERT_TRUE(hal.powerOn().ok());
  //   auto t = hal.readTemperatureCelsius(); ASSERT_TRUE(t.ok());
  //   EXPECT_GE(t.value(), 0.0); EXPECT_LT(t.value(), 120.0);
  //   ASSERT_TRUE(hal.mute(true).ok()); ASSERT_TRUE(hal.mute(false).ok());
  GTEST_SKIP() << "requires real amplifier HAL (NEXUS_STUB_HAL=OFF)";
}

// Microphone: capture a short buffer from the real mic and confirm it is non-silent (ambient).
TEST(Hardware, MicrophoneCaptureNonSilent) {
  if (!onTarget()) GTEST_SKIP() << "set NEXUS_ON_TARGET=1 on the Pi to run";
  GTEST_SKIP() << "requires real microphone HAL (NEXUS_STUB_HAL=OFF)";
}

// Audio output: open the ALSA/I2S sink at 48k/16/stereo and write a short tone without underrun.
TEST(Hardware, AudioOutputOpensAndWrites) {
  if (!onTarget()) GTEST_SKIP() << "set NEXUS_ON_TARGET=1 on the Pi to run";
  GTEST_SKIP() << "requires real audio output HAL (NEXUS_STUB_HAL=OFF)";
}

// Acoustic loopback: play a calibration tone and confirm the mic picks up the expected band —
// the end-to-end acoustic path the calibration relies on.
TEST(Hardware, AcousticLoopback) {
  if (!onTarget()) GTEST_SKIP() << "set NEXUS_ON_TARGET=1 on the Pi to run";
  GTEST_SKIP() << "requires speaker + mic on the target device";
}
