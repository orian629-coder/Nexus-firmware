#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "identity/DeviceIdentity.h"
#include "status/StatusService.h"
#include "storage/SecureStorage.h"

using namespace nexus;
namespace fs = std::filesystem;

namespace {
fs::path sandbox(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_stat_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
}  // namespace

TEST(StatusService, BuildsHeartbeatWithDeviceState) {
  auto dir = sandbox("hb");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  id.start();
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();
  config.update([](config::SpeakerConfig& c) { c.audio.volume = 55; });

  status::StatusService svc(&bus, &id, &config, [] { return 12345; });
  auto hb = svc.buildHeartbeat();
  EXPECT_EQ(hb["type"], "heartbeat");
  EXPECT_EQ(hb["device_id"], id.deviceId());
  EXPECT_EQ(hb["volume"].get<int>(), 55);
  EXPECT_EQ(hb["timestamp"].get<int>(), 12345);
}

TEST(StatusService, HeartbeatDispatchedToSink) {
  auto dir = sandbox("sink");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  id.start();
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();

  std::vector<nlohmann::json> sent;
  std::mutex m;
  status::StatusService svc(
      &bus, &id, &config, [] { return 1; },
      [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lk(m);
        sent.push_back(j);
      });
  svc.sendHeartbeat();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0]["type"], "heartbeat");
  EXPECT_EQ(svc.heartbeatCount(), 1u);
}

TEST(StatusService, CriticalEventTriggersImmediateAlert) {
  auto dir = sandbox("alert");
  core::EventBus bus;
  storage::SecureStorage secrets((dir / "secure").string());
  identity::DeviceIdentity id((dir / "identity/factory.json").string(), &secrets, &bus);
  id.start();
  config::ConfigManager config((dir / "config.json").string(), &bus);
  config.start();

  std::vector<nlohmann::json> sent;
  std::mutex m;
  // Long heartbeat interval so the only dispatch we see is the alert.
  status::StatusService svc(
      &bus, &id, &config, [] { return 1; },
      [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lk(m);
        sent.push_back(j);
      },
      /*heartbeat_seconds=*/3600);
  ASSERT_TRUE(svc.start().ok());

  bus.publish(core::Event{core::EventType::AmplifierOverheat, "amplifier"});
  bus.drain();

  svc.stop();
  std::lock_guard<std::mutex> lk(m);
  ASSERT_FALSE(sent.empty());
  bool found = false;
  for (auto& j : sent) {
    if (j.value("type", "") == "alert" && j.value("alert", "") == "AMPLIFIER_OVERHEAT") found = true;
  }
  EXPECT_TRUE(found);
}
