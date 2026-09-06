#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "config/ConfigManager.h"
#include "config/ConfigSerialization.h"
#include "config/ConfigValidator.h"
#include "config/DefaultConfig.h"

using namespace nexus::config;
namespace fs = std::filesystem;

namespace {
fs::path freshDir(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_cfg_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
}  // namespace

TEST(ConfigValidator, AcceptsDefaults) {
  EXPECT_TRUE(ConfigValidator::validate(defaults()).ok());
}

TEST(ConfigValidator, RejectsBadValues) {
  auto c = defaults();
  c.audio.volume = 150;
  EXPECT_FALSE(ConfigValidator::validate(c).ok());

  c = defaults();
  c.network.connection_mode = "carrier-pigeon";
  EXPECT_FALSE(ConfigValidator::validate(c).ok());

  c = defaults();
  c.pairing.paired = true;
  c.pairing.streamer_id = "";
  EXPECT_FALSE(ConfigValidator::validate(c).ok());
}

TEST(ConfigManager, SeedsDefaultsOnFirstBoot) {
  auto dir = freshDir("seed");
  auto path = (dir / "config.json").string();
  ConfigManager mgr(path);
  ASSERT_TRUE(mgr.start().ok());
  EXPECT_TRUE(fs::exists(path));
  EXPECT_EQ(mgr.get().audio.volume, 30);
}

TEST(ConfigManager, UpdatePersistsAtomically) {
  auto dir = freshDir("update");
  auto path = (dir / "config.json").string();
  ConfigManager mgr(path);
  ASSERT_TRUE(mgr.start().ok());

  ASSERT_TRUE(mgr.update([](SpeakerConfig& c) { c.audio.volume = 55; }).ok());
  EXPECT_EQ(mgr.get().audio.volume, 55);

  // Reload from disk in a fresh manager to confirm it persisted.
  ConfigManager mgr2(path);
  ASSERT_TRUE(mgr2.start().ok());
  EXPECT_EQ(mgr2.get().audio.volume, 55);

  // No leftover temp file.
  EXPECT_FALSE(fs::exists(path + ".tmp"));
}

TEST(ConfigManager, RejectsInvalidUpdateLeavesDiskUnchanged) {
  auto dir = freshDir("reject");
  auto path = (dir / "config.json").string();
  ConfigManager mgr(path);
  ASSERT_TRUE(mgr.start().ok());
  ASSERT_TRUE(mgr.update([](SpeakerConfig& c) { c.audio.volume = 42; }).ok());

  auto bad = mgr.update([](SpeakerConfig& c) { c.audio.volume = 9999; });
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(mgr.get().audio.volume, 42);  // memory unchanged

  ConfigManager mgr2(path);
  ASSERT_TRUE(mgr2.start().ok());
  EXPECT_EQ(mgr2.get().audio.volume, 42);  // disk unchanged
}

TEST(ConfigManager, RecoversFromBackupWhenPrimaryCorrupt) {
  auto dir = freshDir("corrupt");
  auto path = (dir / "config.json").string();
  {
    ConfigManager mgr(path);
    ASSERT_TRUE(mgr.start().ok());
    ASSERT_TRUE(mgr.update([](SpeakerConfig& c) { c.audio.volume = 71; }).ok());
    // A second update creates a .bak of the (good) 71 version.
    ASSERT_TRUE(mgr.update([](SpeakerConfig& c) { c.audio.eq_profile = "flat"; }).ok());
  }
  // Corrupt the primary file.
  { std::ofstream(path) << "{ this is not valid json"; }

  ConfigManager mgr(path);
  mgr.start();  // returns non-ok (corruption) but recovers usable config from backup
  // Backup held volume=71 (profile update happened after the backup snapshot).
  EXPECT_EQ(mgr.get().audio.volume, 71);
  EXPECT_TRUE(fs::exists(path + ".corrupt"));
}

TEST(DefaultConfig, ShippedJsonRoundTrips) {
  auto j = defaultJson();
  auto back = j.get<SpeakerConfig>();
  EXPECT_EQ(back.audio.volume, defaults().audio.volume);
  EXPECT_EQ(back.device.model, defaults().device.model);
}
