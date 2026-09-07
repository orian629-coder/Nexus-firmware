#include "provisioning/AutoPairWorker.h"
#include "provisioning/ProvisioningWindow.h"
#include "discovery/IStreamerDiscovery.h"
#include <gtest/gtest.h>
#include <set>
using namespace nexus::streamer;
using nexus::streamer::provisioning::AutoPairWorker;
using nexus::streamer::provisioning::ProvisioningWindow;
using nexus::streamer::provisioning::PairAttempt;
using nexus::streamer::discovery::DiscoveredSpeaker;

namespace {
struct FakeDiscovery : nexus::streamer::discovery::IStreamerDiscovery {
  std::vector<DiscoveredSpeaker> speakers;
  nexus::core::Status advertise(const nexus::streamer::discovery::StreamerAdvertisement&) override {
    return nexus::core::Status::success();
  }
  nexus::core::Status stopAdvertising() override { return nexus::core::Status::success(); }
  nexus::core::Result<std::vector<DiscoveredSpeaker>> browseSpeakers() override { return speakers; }
};
DiscoveredSpeaker spk(const std::string& id) {
  DiscoveredSpeaker d; d.device_id = id; d.host = id + ".local";
  d.box_public_key = "BOXKEY"; d.control_port = 45455; d.setup_mode = true; return d;
}
}  // namespace

TEST(DeriveSetupCode, MatchesSpeakerRule) {
  EXPECT_EQ(provisioning::deriveSetupCode("SPK-a1b2c3d4"), "SETUP-a1b2c3d4");
  EXPECT_EQ(provisioning::deriveSetupCode("bad"), "");
}

TEST(AutoPairWorker, PairsNewSpeakerWhenOpen) {
  FakeDiscovery disc; disc.speakers = { spk("SPK-a1b2c3d4") };
  ProvisioningWindow win; win.open(1000, 600);
  std::set<std::string> registered;
  std::vector<PairAttempt> attempts;
  AutoPairWorker w(disc, win,
    /*isRegistered=*/[&](const std::string& id){ return registered.count(id) > 0; },
    /*pair=*/[&](const PairAttempt& a){ attempts.push_back(a); registered.insert(a.device_id); return true; },
    /*now=*/[]{ return (std::int64_t)1000; });
  EXPECT_EQ(w.sweepOnce(), 1);
  ASSERT_EQ(attempts.size(), 1u);
  EXPECT_EQ(attempts[0].device_id, "SPK-a1b2c3d4");
  EXPECT_EQ(attempts[0].setup_code, "SETUP-a1b2c3d4");
  EXPECT_EQ(attempts[0].box_public_key, "BOXKEY");
  EXPECT_EQ(w.sweepOnce(), 0);   // already registered → not re-paired
}

TEST(AutoPairWorker, NoOpWhenClosed) {
  FakeDiscovery disc; disc.speakers = { spk("SPK-a1b2c3d4") };
  ProvisioningWindow win;  // closed
  AutoPairWorker w(disc, win, [](auto&){return false;}, [](auto&){return true;}, []{return (std::int64_t)1000;});
  EXPECT_EQ(w.sweepOnce(), 0);
}

TEST(AutoPairWorker, SkipsNonSetupModeAndBadIds) {
  FakeDiscovery disc;
  auto paired = spk("SPK-deadbeef"); paired.setup_mode = false;      // not in setup mode
  auto badid = spk("ROGUE"); // malformed device_id → no setup code
  disc.speakers = { paired, badid };
  ProvisioningWindow win; win.open(1000, 600);
  int calls = 0;
  AutoPairWorker w(disc, win, [](auto&){return false;},
                   [&](auto&){ ++calls; return true; }, []{return (std::int64_t)1000;});
  EXPECT_EQ(w.sweepOnce(), 0);
  EXPECT_EQ(calls, 0);
}
