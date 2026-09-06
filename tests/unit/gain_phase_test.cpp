// Per-speaker trim and polarity: SET_GAIN and SET_PHASE_INVERT.
//
// Both of these previously either did nothing (SET_GAIN was acknowledged with deferred:true and
// never executed) or did not exist (phase invert). A command that reports success without acting is
// worse than one that fails: the UI shows "applied" and the installer goes looking for the fault
// somewhere else.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config/ConfigManager.h"
#include "control/CommandExecutor.h"
#include "core/EventBus.h"
#include "dsp/DspEngine.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

// A config manager over a scratch dir, so each test starts from defaults.
struct Fixture {
  fs::path dir;
  std::unique_ptr<config::ConfigManager> config;
  core::EventBus bus;

  explicit Fixture(const std::string& name) {
    dir = fs::temp_directory_path() / ("nexus-gain-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    config = std::make_unique<config::ConfigManager>((dir / "config.json").string());
    config->load();
  }
  ~Fixture() { fs::remove_all(dir); }
};

control::Command make(const std::string& cmd, nlohmann::json payload) {
  control::Command c;
  c.command = cmd;
  c.command_id = "test-" + cmd;
  c.payload = std::move(payload);
  return c;
}

}  // namespace

// ── SET_GAIN ──

TEST(SetGain, IsExecutedNotMerelyAcknowledged) {
  Fixture f("exec");
  control::CommandExecutor ex(&f.bus, f.config.get());

  const auto r = ex.execute(make(control::cmd::kSetGain, {{"gain_db", 4.5}}));

  ASSERT_TRUE(r.ok) << r.message;
  // The old behaviour: {"accepted":true,"deferred":true} with config untouched.
  EXPECT_FALSE(r.data.contains("deferred"))
      << "SET_GAIN must actually apply, not be deferred to a module that never arrived";
  EXPECT_DOUBLE_EQ(r.data["gain_db"].get<double>(), 4.5);
  EXPECT_DOUBLE_EQ(f.config->get().audio.gain_db, 4.5) << "the value must reach config";
}

TEST(SetGain, IsClampedAndReportsWhatWasApplied) {
  Fixture f("clamp");
  control::CommandExecutor ex(&f.bus, f.config.get());

  // An overshoot is a slider going too far, not a protocol error — clamp and report the truth so
  // the UI can snap back to the applied value.
  const auto hi = ex.execute(make(control::cmd::kSetGain, {{"gain_db", 99.0}}));
  ASSERT_TRUE(hi.ok);
  EXPECT_DOUBLE_EQ(hi.data["gain_db"].get<double>(), 20.0);
  EXPECT_DOUBLE_EQ(f.config->get().audio.gain_db, 20.0);

  const auto lo = ex.execute(make(control::cmd::kSetGain, {{"gain_db", -99.0}}));
  ASSERT_TRUE(lo.ok);
  EXPECT_DOUBLE_EQ(lo.data["gain_db"].get<double>(), -20.0);
}

TEST(SetGain, RejectsAMissingOrNonNumericValue) {
  Fixture f("bad");
  control::CommandExecutor ex(&f.bus, f.config.get());

  EXPECT_FALSE(ex.execute(make(control::cmd::kSetGain, nlohmann::json::object())).ok);
  EXPECT_FALSE(ex.execute(make(control::cmd::kSetGain, {{"gain_db", "loud"}})).ok);
  EXPECT_DOUBLE_EQ(f.config->get().audio.gain_db, 0.0) << "a rejected command must change nothing";
}

// ── SET_PHASE_INVERT ──

TEST(SetPhaseInvert, IsExecutedAndPersisted) {
  Fixture f("phase");
  control::CommandExecutor ex(&f.bus, f.config.get());

  const auto on = ex.execute(make(control::cmd::kSetPhaseInvert, {{"phase_invert", true}}));
  ASSERT_TRUE(on.ok) << on.message;
  EXPECT_TRUE(on.data["phase_invert"].get<bool>());
  EXPECT_TRUE(f.config->get().audio.phase_invert);

  const auto off = ex.execute(make(control::cmd::kSetPhaseInvert, {{"phase_invert", false}}));
  ASSERT_TRUE(off.ok);
  EXPECT_FALSE(f.config->get().audio.phase_invert);
}

TEST(SetPhaseInvert, RejectsANonBoolean) {
  Fixture f("phasebad");
  control::CommandExecutor ex(&f.bus, f.config.get());
  EXPECT_FALSE(ex.execute(make(control::cmd::kSetPhaseInvert, {{"phase_invert", 1}})).ok);
}

// ── GET_STATUS reports both, so the streamer can confirm rather than assume ──

TEST(GetStatus, ReportsGainAndPhase) {
  Fixture f("status");
  control::CommandExecutor ex(&f.bus, f.config.get());
  ex.execute(make(control::cmd::kSetGain, {{"gain_db", -3.0}}));
  ex.execute(make(control::cmd::kSetPhaseInvert, {{"phase_invert", true}}));

  const auto r = ex.execute(make(control::cmd::kGetStatus, nlohmann::json::object()));
  ASSERT_TRUE(r.ok);
  EXPECT_DOUBLE_EQ(r.data["gain_db"].get<double>(), -3.0);
  EXPECT_TRUE(r.data["phase_invert"].get<bool>());
}

// ── the DSP actually inverts ──

TEST(DspPhaseInvert, FlipsTheSignOfEverySample) {
  dsp::DspEngine engine(nullptr, 48000.0, 2);
  engine.start();
  engine.setBypass(true);  // isolate polarity from any tone shaping
  engine.setPhaseInvert(true);

  std::vector<std::int16_t> pcm{1000, -2000, 3000, -4000};
  const auto original = pcm;
  engine.processInt16(pcm.data(), 2, 2);

  for (std::size_t i = 0; i < pcm.size(); ++i) {
    EXPECT_EQ(pcm[i], -original[i]) << "sample " << i;
  }
  engine.stop();
}

// Bypass means "no tone shaping", not "no polarity correction": a driver wired backwards is still
// wired backwards. Dropping the inversion under bypass would silently reintroduce the bass
// cancellation the installer had just corrected.
TEST(DspPhaseInvert, StillAppliesUnderBypass) {
  dsp::DspEngine engine(nullptr, 48000.0, 2);
  engine.start();
  engine.setBypass(true);
  engine.setPhaseInvert(true);

  std::vector<std::int16_t> pcm{500, 500};
  engine.processInt16(pcm.data(), 1, 2);
  EXPECT_EQ(pcm[0], -500);
  EXPECT_EQ(pcm[1], -500);
  engine.stop();
}

// -32768 has no positive counterpart in int16: negating it overflows back to itself, which would
// leave the loudest samples at full scale with the WRONG sign — an audible click exactly where the
// signal is loudest.
TEST(DspPhaseInvert, HandlesTheMostNegativeSampleWithoutOverflow) {
  dsp::DspEngine engine(nullptr, 48000.0, 2);
  engine.start();
  engine.setBypass(true);
  engine.setPhaseInvert(true);

  std::vector<std::int16_t> pcm{-32768, -32768};
  engine.processInt16(pcm.data(), 1, 2);
  EXPECT_EQ(pcm[0], 32767) << "must clamp, not wrap back to -32768";
  EXPECT_EQ(pcm[1], 32767);
  engine.stop();
}

TEST(DspPhaseInvert, IsOffByDefault) {
  dsp::DspEngine engine(nullptr, 48000.0, 2);
  engine.start();
  engine.setBypass(true);

  std::vector<std::int16_t> pcm{1234, -1234};
  engine.processInt16(pcm.data(), 1, 2);
  EXPECT_EQ(pcm[0], 1234);
  EXPECT_EQ(pcm[1], -1234);
  engine.stop();
}
