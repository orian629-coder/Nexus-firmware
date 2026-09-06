// Speaker telemetry: playback health carried in GET_STATUS and mirrored in the streamer's store.
//
// The rule these tests defend: the streamer must be able to tell "this speaker reported no
// telemetry" apart from "this speaker reported all-zero telemetry". Zeros mean a flawless stream;
// silence means we know nothing. Collapsing the two would paint a healthy meter for a speaker that
// is not playing at all.

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "state/SpeakerStateStore.h"

using namespace nexus::streamer;

namespace {

// A GET_STATUS reply carrying the fields a speaker with an audio pipeline reports.
nlohmann::json statusWithTelemetry(double loss_pct, std::uint64_t underflows) {
  return {{"volume", 50},
          {"muted", false},
          {"delay_ms", 0},
          {"eq_profile", "default"},
          {"paired", true},
          {"telemetry",
           {{"buffer_depth", 12},
            {"packets_received", 1000},
            {"packets_lost", 3},
            {"packet_loss_pct", loss_pct},
            {"underflows", underflows},
            {"dropped_overflow", 0},
            {"latency_ms", 8.5},
            {"clock_offset_ms", -1.2},
            {"drift_ppm", 4.0}}}};
}

}  // namespace

TEST(Telemetry, IsStoredAndReported) {
  state::SpeakerStateStore store;
  store.applyConfirmedStatus("SPK-1", statusWithTelemetry(0.3, 2));

  const auto j = store.get("SPK-1")->toJson(std::chrono::steady_clock::now());
  ASSERT_TRUE(j.contains("telemetry"));
  EXPECT_EQ(j["telemetry"]["buffer_depth"], 12);
  EXPECT_EQ(j["telemetry"]["packets_lost"], 3);
  EXPECT_DOUBLE_EQ(j["telemetry"]["packet_loss_pct"].get<double>(), 0.3);
  EXPECT_EQ(j["telemetry"]["underflows"], 2);
  EXPECT_DOUBLE_EQ(j["telemetry"]["latency_ms"].get<double>(), 8.5);
}

// The distinction this whole feature rests on.
TEST(Telemetry, AbsentTelemetryIsOmittedRatherThanZeroed) {
  state::SpeakerStateStore store;
  // Older firmware: a valid status reply with no telemetry section at all.
  store.applyConfirmedStatus("SPK-OLD", {{"volume", 50}, {"muted", false}, {"paired", true}});

  const auto j = store.get("SPK-OLD")->toJson(std::chrono::steady_clock::now());
  EXPECT_TRUE(j["confirmed"].get<bool>()) << "the speaker did confirm its settings";
  EXPECT_FALSE(j.contains("telemetry"))
      << "no telemetry must be absent, not reported as a healthy stream of zeros";
}

// A genuinely perfect stream reports zeros, and those zeros must survive.
TEST(Telemetry, ZeroLossIsReportedAsRealData) {
  state::SpeakerStateStore store;
  store.applyConfirmedStatus("SPK-1", statusWithTelemetry(0.0, 0));

  const auto j = store.get("SPK-1")->toJson(std::chrono::steady_clock::now());
  ASSERT_TRUE(j.contains("telemetry"));
  EXPECT_DOUBLE_EQ(j["telemetry"]["packet_loss_pct"].get<double>(), 0.0);
  EXPECT_EQ(j["telemetry"]["underflows"], 0);
}

// Unknown fields must pass through, so newer speaker firmware can add metrics without requiring a
// streamer change.
TEST(Telemetry, UnknownFieldsPassThrough) {
  state::SpeakerStateStore store;
  store.applyConfirmedStatus(
      "SPK-1", {{"volume", 50}, {"telemetry", {{"buffer_depth", 4}, {"future_metric", 42}}}});

  const auto j = store.get("SPK-1")->toJson(std::chrono::steady_clock::now());
  EXPECT_EQ(j["telemetry"]["future_metric"], 42);
}

TEST(Telemetry, MalformedTelemetryIsIgnored) {
  state::SpeakerStateStore store;
  // A scalar where an object belongs must not be stored or crash the poll path.
  store.applyConfirmedStatus("SPK-1", {{"volume", 50}, {"telemetry", "broken"}});

  const auto j = store.get("SPK-1")->toJson(std::chrono::steady_clock::now());
  EXPECT_FALSE(j.contains("telemetry"));
  EXPECT_EQ(j["volume"], 50) << "the rest of the reply must still be applied";
}

// ── link strength ──

TEST(Telemetry, LinkStrengthIsReportedWithItsAge) {
  state::SpeakerStateStore store;
  store.applyConfirmedStatus("SPK-1",
                             {{"volume", 50}, {"wifi_signal_dbm", -55}, {"link_age_s", 3}});

  const auto j = store.get("SPK-1")->toJson(std::chrono::steady_clock::now());
  EXPECT_EQ(j["wifi_signal_dbm"], -55);
  EXPECT_EQ(j["link_age_s"], 3);
}

TEST(Telemetry, NoLinkReportMeansNoSignalField) {
  state::SpeakerStateStore store;
  store.applyConfirmedStatus("SPK-1", {{"volume", 50}});

  const auto j = store.get("SPK-1")->toJson(std::chrono::steady_clock::now());
  EXPECT_FALSE(j.contains("wifi_signal_dbm"))
      << "a speaker that never heard a REPORT_LINK must not appear to have a measured link";
}

// A failed poll must not silently keep serving old health numbers as if they were current. The
// store leaves confirmed values alone (a failure says nothing about settings), but `age_ms` is what
// tells the UI they are stale — so it must keep growing.
TEST(Telemetry, StaleTelemetrySurvivesButAges) {
  state::SpeakerStateStore store;
  store.applyConfirmedStatus("SPK-1", statusWithTelemetry(0.1, 0));
  store.applyPollFailure("SPK-1", "timeout");

  const auto later = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const auto j = store.get("SPK-1")->toJson(later);
  ASSERT_TRUE(j.contains("telemetry")) << "last known health is still the last known health";
  EXPECT_GE(j["age_ms"].get<std::int64_t>(), 30000)
      << "age must reveal that these numbers are stale";
}
