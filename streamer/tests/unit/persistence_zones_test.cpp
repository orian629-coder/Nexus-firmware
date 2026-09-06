#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "config/StreamerConfigManager.h"
#include "core/EventBus.h"
#include "group/ZoneManager.h"
#include "sources/SourceFactory.h"
#include "sources/WavFileSource.h"
#include "state/SpeakerStateStore.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {

fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_streamer_persist_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

// Write a minimal 16-bit PCM WAV so the file source can be exercised without a fixture binary.
void writeWav(const fs::path& path, int rate, int channels,
              const std::vector<std::int16_t>& samples) {
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
  f.write("RIFF", 4);
  u32(36 + data_bytes);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  u32(16);
  u16(1);                                                 // PCM
  u16(static_cast<std::uint16_t>(channels));
  u32(static_cast<std::uint32_t>(rate));
  u32(static_cast<std::uint32_t>(rate * channels * 2));   // byte rate
  u16(static_cast<std::uint16_t>(channels * 2));          // block align
  u16(16);                                                // bits
  f.write("data", 4);
  u32(data_bytes);
  f.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
}

}  // namespace

// ── persistence ─────────────────────────────────────────────────────────────────────────────────
//
// The speaker list used to live only in memory, so every restart forgot which speakers were paired.

TEST(StreamerConfig, SpeakersAndZonesSurviveARestart) {
  const auto dir = sandbox("roundtrip");
  const std::string path = (dir / "config.json").string();

  {
    streamer::config::StreamerConfigManager cfg(path);
    ASSERT_TRUE(cfg.start().ok());
    cfg.update([](streamer::config::StreamerConfig& c) {
      c.speakers.push_back({"SPK-1", "Salon", "10.0.0.5", 45455, "", "", 0});
      c.speakers.push_back({"SPK-2", "Kitchen", "10.0.0.6", 45455, "", "", 12});
      c.zones.push_back({"zone-1", "קומה ראשונה", {"SPK-1", "SPK-2"}});
      c.web.auth_token = "tok-abc";
    });
  }  // destroyed — simulates a process restart

  streamer::config::StreamerConfigManager reloaded(path);
  ASSERT_TRUE(reloaded.start().ok());
  const auto c = reloaded.get();
  ASSERT_EQ(c.speakers.size(), 2u);
  EXPECT_EQ(c.speakers[0].device_id, "SPK-1");
  EXPECT_EQ(c.speakers[1].host, "10.0.0.6");
  EXPECT_EQ(c.speakers[1].delay_ms, 12);
  ASSERT_EQ(c.zones.size(), 1u);
  EXPECT_EQ(c.zones[0].members.size(), 2u);
  EXPECT_EQ(c.web.auth_token, "tok-abc") << "the API token must survive a restart";
}

TEST(StreamerConfig, RecoversFromACorruptFileUsingTheBackup) {
  const auto dir = sandbox("corrupt");
  const std::string path = (dir / "config.json").string();

  {
    streamer::config::StreamerConfigManager cfg(path);
    ASSERT_TRUE(cfg.start().ok());
    cfg.update([](streamer::config::StreamerConfig& c) {
      c.speakers.push_back({"SPK-1", "Salon", "10.0.0.5", 45455, "", "", 0});
    });
    // A second write rolls the good file into .bak.
    cfg.update([](streamer::config::StreamerConfig& c) { c.system.log_level = "debug"; });
  }
  ASSERT_TRUE(fs::exists(path + ".bak"));

  // Truncated write, as a power cut mid-save would leave it.
  { std::ofstream(path) << "{\"speakers\": [ {\"device"; }

  streamer::config::StreamerConfigManager recovered(path);
  ASSERT_TRUE(recovered.start().ok());
  EXPECT_EQ(recovered.get().speakers.size(), 1u) << "did not recover the speaker list";
  EXPECT_TRUE(fs::exists(path + ".corrupt")) << "damaged file should be kept for inspection";
}

TEST(StreamerConfig, PartialConfigLoadsWithDefaults) {
  const auto dir = sandbox("partial");
  const std::string path = (dir / "config.json").string();
  { std::ofstream(path) << R"({"speakers":[{"device_id":"SPK-9","host":"10.0.0.9"}]})"; }

  streamer::config::StreamerConfigManager cfg(path);
  ASSERT_TRUE(cfg.start().ok());
  const auto c = cfg.get();
  ASSERT_EQ(c.speakers.size(), 1u);
  EXPECT_EQ(c.speakers[0].control_port, 45455) << "missing field should take its default";
  EXPECT_EQ(c.audio.sample_rate, 48000);
  EXPECT_EQ(c.web.bind_address, "127.0.0.1");
}

// ── zones ───────────────────────────────────────────────────────────────────────────────────────

TEST(Zones, AudioSkipsOfflineMembersButCommandsDoNot) {
  core::EventBus bus;
  streamer::group::SpeakerRegistry reg;
  streamer::state::SpeakerStateStore store(&bus);
  reg.upsert({"SPK-1", "Salon", "10.0.0.5", 45455});
  reg.upsert({"SPK-2", "Kitchen", "10.0.0.6", 45455});

  streamer::group::ZoneManager zones(reg, &store);
  auto z = zones.create("Ground Floor");
  ASSERT_TRUE(z.ok());
  ASSERT_TRUE(zones.addMember(z.value().zone_id, "SPK-1").ok());
  ASSERT_TRUE(zones.addMember(z.value().zone_id, "SPK-2").ok());

  // Both reachable → both get audio.
  store.applyConfirmedStatus("SPK-1", {{"volume", 30}});
  store.applyConfirmedStatus("SPK-2", {{"volume", 30}});
  EXPECT_EQ(zones.resolveTargets(z.value().zone_id).value().size(), 2u);

  // SPK-2 dies. Audio targets drop it (streaming to a dead host is wasted bandwidth)...
  for (int i = 0; i < 3; ++i) store.applyPollFailure("SPK-2", "timeout", 3);
  auto targets = zones.resolveTargets(z.value().zone_id);
  ASSERT_TRUE(targets.ok());
  ASSERT_EQ(targets.value().size(), 1u);
  EXPECT_EQ(targets.value()[0].host, "10.0.0.5");

  // ...but commands still address it, so the user sees a real per-speaker error.
  EXPECT_EQ(zones.members(z.value().zone_id).size(), 2u)
      << "an offline speaker must still receive commands so its failure is visible";
}

// Found on a running streamer: after a restart, creating a zone produced a SECOND zone with
// zone_id "zone-1", because the id counter lives in memory and starts at 1 while the existing zones
// come back from disk. Every lookup matches by id, so remove/rename/play would silently act on the
// wrong zone.
TEST(Zones, IdsDoNotCollideWithZonesRestoredFromDisk) {
  streamer::group::SpeakerRegistry reg;
  streamer::group::ZoneManager zones(reg);

  // Simulate a restart: zones loaded from config, with a fresh (counter == 1) manager.
  zones.replaceAll({{"zone-1", "Salon", {}}, {"zone-2", "Kitchen", {}}});

  auto created = zones.create("New Room");
  ASSERT_TRUE(created.ok());
  EXPECT_NE(created.value().zone_id, "zone-1");
  EXPECT_NE(created.value().zone_id, "zone-2");

  // And the ids must all still be distinct.
  const auto all = zones.list();
  ASSERT_EQ(all.size(), 3u);
  std::vector<std::string> ids;
  for (const auto& z : all) ids.push_back(z.zone_id);
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(std::unique(ids.begin(), ids.end()), ids.end()) << "duplicate zone ids";

  // Removing the new zone must not take a restored one with it.
  ASSERT_TRUE(zones.remove(created.value().zone_id));
  EXPECT_EQ(zones.list().size(), 2u);
  EXPECT_TRUE(zones.get("zone-1").has_value());
  EXPECT_TRUE(zones.get("zone-2").has_value());
}

TEST(Zones, ASpeakerCannotBelongToTwoZones) {
  streamer::group::SpeakerRegistry reg;
  reg.upsert({"SPK-1", "Salon", "10.0.0.5", 45455});
  streamer::group::ZoneManager zones(reg);

  auto a = zones.create("A");
  auto b = zones.create("B");
  ASSERT_TRUE(zones.addMember(a.value().zone_id, "SPK-1").ok());

  auto s = zones.addMember(b.value().zone_id, "SPK-1");
  EXPECT_FALSE(s.ok()) << "two zones streaming to one speaker would produce garbage";
  EXPECT_NE(s.message().find("already belongs"), std::string::npos);

  // Moving it is fine once removed from the first.
  ASSERT_TRUE(zones.removeMember(a.value().zone_id, "SPK-1").ok());
  EXPECT_TRUE(zones.addMember(b.value().zone_id, "SPK-1").ok());
  EXPECT_EQ(zones.zoneOf("SPK-1").value(), b.value().zone_id);
}

TEST(Zones, UnknownSpeakerOrZoneIsRejected) {
  streamer::group::SpeakerRegistry reg;
  streamer::group::ZoneManager zones(reg);
  auto z = zones.create("A");
  EXPECT_FALSE(zones.addMember(z.value().zone_id, "SPK-GHOST").ok());
  EXPECT_FALSE(zones.addMember("zone-nope", "SPK-1").ok());
  EXPECT_FALSE(zones.resolveTargets("zone-nope").ok());
}

// ── audio sources ───────────────────────────────────────────────────────────────────────────────
//
// Until now the streamer could only read raw PCM from stdin, so it could not open a file itself.

TEST(Sources, WavFileAt48kRoundTripsExactly) {
  const auto dir = sandbox("wav48");
  const auto path = dir / "tone.wav";
  std::vector<std::int16_t> pcm;
  for (int i = 0; i < 480 * 2; ++i) pcm.push_back(static_cast<std::int16_t>(i % 3000));
  writeWav(path, 48000, 2, pcm);

  auto src = streamer::sources::WavFileSource::open(path.string());
  ASSERT_TRUE(src.ok()) << src.status().message();

  std::vector<std::int16_t> out;
  const std::size_t frames = src.value()->read(480, out);
  EXPECT_EQ(frames, 480u);
  ASSERT_EQ(out.size(), 960u);
  // No resampling at the wire rate, so samples must be bit-exact.
  for (std::size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i], pcm[i]) << "at sample " << i;
}

TEST(Sources, MonoWavIsWidenedToStereo) {
  const auto dir = sandbox("wavmono");
  const auto path = dir / "mono.wav";
  const std::vector<std::int16_t> pcm{100, 200, 300, 400};
  writeWav(path, 48000, 1, pcm);

  auto src = streamer::sources::WavFileSource::open(path.string());
  ASSERT_TRUE(src.ok());
  std::vector<std::int16_t> out;
  ASSERT_EQ(src.value()->read(4, out), 4u);
  ASSERT_EQ(out.size(), 8u);
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    EXPECT_EQ(out[i * 2], pcm[i]);
    EXPECT_EQ(out[i * 2 + 1], pcm[i]) << "mono should be duplicated to both channels";
  }
}

TEST(Sources, NonWireRateIsResampledToTheWireRate) {
  const auto dir = sandbox("wav24k");
  const auto path = dir / "half.wav";
  std::vector<std::int16_t> pcm;
  for (int i = 0; i < 1000 * 2; ++i) pcm.push_back(static_cast<std::int16_t>((i / 2) % 500));
  writeWav(path, 24000, 2, pcm);  // half rate → roughly twice as many output frames

  auto src = streamer::sources::WavFileSource::open(path.string());
  ASSERT_TRUE(src.ok());
  EXPECT_EQ(src.value()->sourceSampleRate(), 24000);
  EXPECT_EQ(src.value()->sampleRate(), 48000) << "source must present the wire rate";

  std::size_t total = 0;
  std::vector<std::int16_t> out;
  while (!src.value()->exhausted() && total < 4000) {
    out.clear();
    const std::size_t n = src.value()->read(480, out);
    if (n == 0) break;
    total += n;
    EXPECT_EQ(out.size(), n * 2) << "output must stay stereo";
  }
  // 1000 source frames at 24k ≈ 2000 frames at 48k; allow slack for interpolation edges.
  EXPECT_GT(total, 1800u);
  EXPECT_LT(total, 2100u);
}

TEST(Sources, BadFilesAreRejectedWithAMessageNotNoise) {
  EXPECT_FALSE(streamer::sources::WavFileSource::open("/nonexistent/none.wav").ok());

  const auto dir = sandbox("notwav");
  const auto path = dir / "fake.wav";
  { std::ofstream(path) << "this is not a wav file at all"; }
  auto r = streamer::sources::WavFileSource::open(path.string());
  EXPECT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("RIFF"), std::string::npos);
}

TEST(Sources, FactoryBuildsFileSourcesAndRejectsUnknownKinds) {
  const auto dir = sandbox("factory");
  const auto path = dir / "a.wav";
  writeWav(path, 48000, 2, std::vector<std::int16_t>(960, 42));

  auto made = streamer::sources::makeSource({"file", path.string()});
  ASSERT_TRUE(made.ok()) << made.status().message();
  std::vector<std::int16_t> out;
  EXPECT_GT(made.value()->read(480, out), 0u);

  EXPECT_FALSE(streamer::sources::makeSource({"file", ""}).ok());
  EXPECT_FALSE(streamer::sources::makeSource({"airplay", "x"}).ok());
}

// A path containing a quote must not be able to break out of the shell argument.
TEST(Sources, ShellQuotingNeutralizesEmbeddedQuotes) {
  const std::string quoted = streamer::sources::shellQuote("a'b; rm -rf /");
  EXPECT_EQ(quoted.front(), '\'');
  EXPECT_EQ(quoted.back(), '\'');
  EXPECT_NE(quoted.find("'\\''"), std::string::npos) << "embedded quote was not escaped";
}
