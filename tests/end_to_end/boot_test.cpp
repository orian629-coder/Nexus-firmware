// End-to-end boot smoke test: construct the full Application with all real+stub services, start
// them in order, confirm the state machine reached a sane state, then shut down cleanly.

#include <gtest/gtest.h>

#include <filesystem>

#include "main/Application.h"

namespace fs = std::filesystem;
using namespace nexus;

namespace {
AppOptions sandboxOpts(const std::string& name) {
  auto d = fs::temp_directory_path() / ("nexus_e2e_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  AppOptions o;
  o.config_path = (d / "config.json").string();
  o.identity_path = (d / "identity/factory.json").string();
  o.secure_dir = (d / "secure").string();
  o.log_path = (d / "speaker.log").string();
  return o;
}
}  // namespace

TEST(EndToEnd, FullBootAndCleanShutdown) {
  Application app(sandboxOpts("boot"));
  ASSERT_TRUE(app.startup().ok());

  // Fresh device (no pairing) enters SetupMode after startup (advertises a beacon + opens a
  // pairing window). It passes through Unconfigured on the way.
  EXPECT_EQ(app.system().states().current(), system::SystemState::SetupMode);
  EXPECT_TRUE(app.pairing().inSetupMode());
  EXPECT_FALSE(app.identity().deviceId().empty());

  app.shutdown();  // reverse-order stop; must not crash or hang
  SUCCEED();
}

TEST(EndToEnd, ConfiguredDeviceEntersConnectingNetwork) {
  auto opts = sandboxOpts("configured");
  {
    // Pre-seed a paired config so startup resolves to ConnectingNetwork.
    Application app(opts);
    ASSERT_TRUE(app.startup().ok());
    ASSERT_TRUE(app.config()
                    .update([](config::SpeakerConfig& c) {
                      c.pairing.paired = true;
                      c.pairing.streamer_id = "STR-TEST";
                    })
                    .ok());
    app.shutdown();
  }
  Application app2(opts);
  ASSERT_TRUE(app2.startup().ok());
  EXPECT_EQ(app2.system().states().current(), system::SystemState::ConnectingNetwork);
  app2.shutdown();
}
