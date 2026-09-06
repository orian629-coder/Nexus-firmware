#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "core/Event.h"
#include "core/EventBus.h"
#include "pairing/ProvisioningController.h"

using namespace nexus;
using nexus::core::Event;
using nexus::core::EventBus;
using nexus::core::EventType;
using Channel = nexus::pairing::ProvisioningController::Channel;

namespace {

// Captures every actuation (per channel) so tests can assert the sequence the controller drives.
struct Recorder {
  std::vector<std::pair<Channel, bool>> calls;
  pairing::ProvisioningController::Actuator actuator() {
    return [this](Channel ch, bool active) { calls.emplace_back(ch, active); };
  }
  // Most recent actuation recorded for a channel (test-fatal if the channel was never actuated).
  bool lastFor(Channel ch) const {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      if (it->first == ch) return it->second;
    }
    ADD_FAILURE() << "channel was never actuated";
    return false;
  }
  int countFor(Channel ch) const {
    int n = 0;
    for (const auto& c : calls) if (c.first == ch) ++n;
    return n;
  }
};

}  // namespace

TEST(ProvisioningController, AdvertisesAtBootWhenNoNetwork) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());

  ASSERT_TRUE(pc.start().ok());
  bus.drain();

  // No connectivity known at boot → BOTH channels on (BLE advertising + Wi-Fi setup hotspot).
  EXPECT_TRUE(pc.advertising());
  EXPECT_TRUE(pc.hotspotOn());
  EXPECT_TRUE(rec.lastFor(Channel::Ble));
  EXPECT_TRUE(rec.lastFor(Channel::Hotspot));
}

TEST(ProvisioningController, BootProbeWithEthernetDropsHotspotImmediately) {
  EventBus bus;
  Recorder rec;
  // Simulate a wired speaker at boot: the initial probe reports an ethernet uplink. The controller
  // must NOT bring the hotspot up (it would deadlock wlan0), even though no NetworkConnected event
  // is ever delivered. BLE stays up (not on Wi-Fi yet).
  auto probe = []() {
    return pairing::ProvisioningController::InitialState{/*have_uplink=*/true, /*on_wifi=*/false};
  };
  pairing::ProvisioningController pc(&bus, rec.actuator(), probe);
  ASSERT_TRUE(pc.start().ok());
  bus.drain();

  EXPECT_TRUE(pc.advertising());   // BLE on
  EXPECT_FALSE(pc.hotspotOn());    // hotspot down
  // The hotspot is asserted DOWN at boot (enforce), never up — so wlan0 is free from boot. Every
  // hotspot actuation must be OFF; there must be no ON.
  ASSERT_GT(rec.countFor(Channel::Hotspot), 0);
  EXPECT_FALSE(rec.lastFor(Channel::Hotspot));
  for (const auto& c : rec.calls) {
    if (c.first == Channel::Hotspot) EXPECT_FALSE(c.second) << "hotspot must never be turned ON here";
  }
}

TEST(ProvisioningController, BootProbeWithWifiStartsNothing) {
  EventBus bus;
  Recorder rec;
  auto probe = []() {
    return pairing::ProvisioningController::InitialState{/*have_uplink=*/true, /*on_wifi=*/true};
  };
  pairing::ProvisioningController pc(&bus, rec.actuator(), probe);
  ASSERT_TRUE(pc.start().ok());
  bus.drain();

  EXPECT_FALSE(pc.advertising());
  EXPECT_FALSE(pc.hotspotOn());
  // On Wi-Fi at boot: BLE is never actuated on (fully provisioned), and the hotspot is only ever
  // asserted DOWN — never turned on. No channel is ever turned ON.
  EXPECT_EQ(rec.countFor(Channel::Ble), 0);
  for (const auto& c : rec.calls) EXPECT_FALSE(c.second) << "no channel should be turned ON at boot on wifi";
}

TEST(ProvisioningController, WifiConnectedStopsBothChannels) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());
  ASSERT_TRUE(pc.start().ok());
  bus.drain();

  // A Wi-Fi uplink fully provisions the speaker: BLE stops (on Wi-Fi) and the hotspot stops (an
  // uplink exists).
  bus.publish(Event{EventType::NetworkConnected, "network", {{"mode", "wifi"}, {"ip", "10.0.0.5"}}});
  bus.drain();

  EXPECT_FALSE(pc.advertising());
  EXPECT_FALSE(pc.hotspotOn());
  EXPECT_FALSE(rec.lastFor(Channel::Ble));
  EXPECT_FALSE(rec.lastFor(Channel::Hotspot));
}

TEST(ProvisioningController, EthernetKeepsBleButDropsHotspot) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());
  ASSERT_TRUE(pc.start().ok());
  bus.drain();

  // A wired link does NOT count as provisioned for BLE (keep it up so the user can move onto
  // Wi-Fi), but the hotspot MUST come down: it owns wlan0 in AP mode and would otherwise block
  // wlan0 from joining a real Wi-Fi network. This is the core of the AP-vs-client fix.
  bus.publish(
      Event{EventType::NetworkConnected, "network", {{"mode", "ethernet"}, {"ip", "10.0.0.5"}}});
  bus.drain();

  EXPECT_TRUE(pc.advertising());        // BLE still on
  EXPECT_FALSE(pc.hotspotOn());         // hotspot released wlan0
  EXPECT_FALSE(rec.lastFor(Channel::Hotspot));
}

TEST(ProvisioningController, HotspotAndBleReturnWhenNetworkDrops) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());
  ASSERT_TRUE(pc.start().ok());
  bus.publish(Event{EventType::NetworkConnected, "network", {{"mode", "wifi"}, {"ip", "10.0.0.5"}}});
  bus.drain();
  ASSERT_FALSE(pc.advertising());
  ASSERT_FALSE(pc.hotspotOn());

  bus.publish(Event{EventType::NetworkDisconnected, "network"});
  bus.drain();

  EXPECT_TRUE(pc.advertising());
  EXPECT_TRUE(pc.hotspotOn());
  EXPECT_TRUE(rec.lastFor(Channel::Ble));
  EXPECT_TRUE(rec.lastFor(Channel::Hotspot));
}

TEST(ProvisioningController, BleIdempotentButHotspotReasserts) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());
  ASSERT_TRUE(pc.start().ok());
  bus.drain();
  const int ble_after_boot = rec.countFor(Channel::Ble);
  const int hs_after_boot = rec.countFor(Channel::Hotspot);

  // Two identical wifi-connect events in a row.
  bus.publish(Event{EventType::NetworkConnected, "network", {{"mode", "wifi"}, {"ip", "10.0.0.5"}}});
  bus.drain();
  bus.publish(Event{EventType::NetworkConnected, "network", {{"mode", "wifi"}, {"ip", "10.0.0.5"}}});
  bus.drain();

  // BLE is idempotent — one off-transition, no redundant actuation.
  EXPECT_EQ(rec.countFor(Channel::Ble), ble_after_boot + 1);
  // The hotspot RE-ASSERTS its desired-down state on every uplink event (belt-and-suspenders so a
  // drifted-up AP self-corrects), so it actuates once per event.
  EXPECT_EQ(rec.countFor(Channel::Hotspot), hs_after_boot + 2);
  EXPECT_FALSE(rec.lastFor(Channel::Hotspot));  // always re-asserted OFF
}

TEST(ProvisioningController, RepeatedUplinkReassertsHotspotDown) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());
  ASSERT_TRUE(pc.start().ok());
  bus.drain();

  // First ethernet event brings the hotspot down (a real transition).
  bus.publish(
      Event{EventType::NetworkConnected, "network", {{"mode", "ethernet"}, {"ip", "10.0.0.5"}}});
  bus.drain();
  const int hs_after_first = rec.countFor(Channel::Hotspot);
  ASSERT_FALSE(pc.hotspotOn());

  // A REPEATED ethernet event (as the monitor now re-publishes every poll) must RE-ASSERT the
  // hotspot-down actuation even though the internal state didn't change — this is what forces the AP
  // back down if it drifted up out of band. So the hotspot actuator is called again.
  bus.publish(
      Event{EventType::NetworkConnected, "network", {{"mode", "ethernet"}, {"ip", "10.0.0.5"}}});
  bus.drain();
  EXPECT_GT(rec.countFor(Channel::Hotspot), hs_after_first);
  EXPECT_FALSE(rec.lastFor(Channel::Hotspot));  // re-asserted OFF
}

TEST(ProvisioningController, StopHaltsBothChannels) {
  EventBus bus;
  Recorder rec;
  pairing::ProvisioningController pc(&bus, rec.actuator());
  ASSERT_TRUE(pc.start().ok());
  bus.drain();
  ASSERT_TRUE(pc.advertising());
  ASSERT_TRUE(pc.hotspotOn());

  ASSERT_TRUE(pc.stop().ok());
  EXPECT_FALSE(pc.advertising());
  EXPECT_FALSE(pc.hotspotOn());
  EXPECT_FALSE(rec.lastFor(Channel::Ble));
  EXPECT_FALSE(rec.lastFor(Channel::Hotspot));
}
