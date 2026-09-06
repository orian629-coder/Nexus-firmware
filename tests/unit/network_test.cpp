#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/EventBus.h"
#include "network/NetworkManager.h"

using namespace nexus::network;
using nexus::core::Event;
using nexus::core::EventBus;
using nexus::core::EventType;

namespace {
// A HAL whose reported status can be flipped between polls, to exercise the connectivity monitor's
// out-of-band detection (the deadlock fix: connectivity that appears/changes AFTER boot must be
// picked up by refresh(), not only by connectWifi()).
class ControllableHal : public INetworkHal {
 public:
  void set(bool connected, std::string mode, std::string ip) {
    std::lock_guard<std::mutex> lk(m_);
    st_.connected = connected;
    st_.mode = std::move(mode);
    st_.ip_address = std::move(ip);
  }
  nexus::core::Result<std::vector<WifiNetwork>> scanWifi() override {
    return std::vector<WifiNetwork>{};
  }
  using INetworkHal::connectWifi;  // keep the ssid+psk convenience overload
  nexus::core::Status connectWifi(const WifiConnectParams&) override {
    if (fail_connect_) {
      return nexus::core::Status::error(nexus::core::ErrorCode::IoError, "nmcli connect failed");
    }
    return nexus::core::Status::success();
  }
  void failConnect(bool fail) { fail_connect_ = fail; }
  nexus::core::Status disconnect() override { return nexus::core::Status::success(); }
  nexus::core::Result<NetworkStatus> status() override {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
  }

 private:
  std::mutex m_;
  NetworkStatus st_;  // starts disconnected
  std::atomic<bool> fail_connect_{false};
};
}  // namespace

// A Wi-Fi join that fails during onboarding must still announce NetworkDisconnected.
//
// This is the stranded-device bug: connectWifi() tears the setup AP down first (one radio cannot be
// AP and client at once), so if the join then fails the hotspot is already gone. The event is what
// makes ProvisioningController bring it back — but publishDisconnected() was edge-triggered, and
// during onboarding the device was never connected, so no edge existed and nothing was published.
// The device ended up with no Wi-Fi and no setup AP: unrecoverable without physical access.
TEST(NetworkManager, FailedConnectAnnouncesDisconnectSoTheHotspotCanRecover) {
  EventBus bus;
  std::atomic<int> disconnects{0};
  bus.subscribe(nexus::core::EventType::NetworkDisconnected,
                [&](const nexus::core::Event&) { disconnects.fetch_add(1); });

  auto hal = std::make_unique<ControllableHal>();
  auto* raw = hal.get();
  NetworkManager net(&bus, std::move(hal));
  net.start();
  ASSERT_FALSE(net.isConnected());  // never connected — exactly the onboarding case

  raw->failConnect(true);
  auto s = net.connectWifi(WifiConnectParams{"Home", "wrong-password"});
  EXPECT_FALSE(s.ok());

  for (int i = 0; i < 100 && disconnects.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(disconnects.load(), 1)
      << "no NetworkDisconnected after a failed join — the setup hotspot would never come back";
  net.stop();
}

// The periodic status poll must stay edge-triggered, or a normally-disconnected device would
// republish the event on every tick and flood both the bus and the journal.
TEST(NetworkManager, SteadyStateDisconnectDoesNotRepublish) {
  EventBus bus;
  std::atomic<int> disconnects{0};
  bus.subscribe(nexus::core::EventType::NetworkDisconnected,
                [&](const nexus::core::Event&) { disconnects.fetch_add(1); });

  auto hal = std::make_unique<ControllableHal>();
  NetworkManager net(&bus, std::move(hal));
  net.start();
  for (int i = 0; i < 5; ++i) net.refresh();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(disconnects.load(), 0) << "steady-state disconnect should not republish";
  net.stop();
}

TEST(NetworkManager, StartStopLifecycle) {
  EventBus bus;
  NetworkManager net(&bus);
  EXPECT_EQ(net.state(), nexus::core::ServiceState::Stopped);
  ASSERT_TRUE(net.start().ok());
  EXPECT_EQ(net.state(), nexus::core::ServiceState::Running);
  ASSERT_TRUE(net.stop().ok());
  EXPECT_EQ(net.state(), nexus::core::ServiceState::Stopped);
}

TEST(NetworkManager, ScanReturnsNetworks) {
  EventBus bus;
  NetworkManager net(&bus);
  net.start();
  auto scan = net.scan();
  ASSERT_TRUE(scan.ok());
  EXPECT_FALSE(scan.value().empty());
}

TEST(NetworkManager, ConnectEmitsNetworkConnected) {
  EventBus bus;
  std::atomic<int> connected{0};
  std::string got_ip;
  bus.subscribe(EventType::NetworkConnected, [&](const Event& e) {
    ++connected;
    got_ip = e.data.value("ip", "");
  });

  NetworkManager net(&bus);
  ASSERT_TRUE(net.start().ok());
  ASSERT_TRUE(net.connectWifi("NexusLab", "password123").ok());
  bus.drain();

  EXPECT_EQ(connected.load(), 1);
  EXPECT_TRUE(net.isConnected());
  EXPECT_FALSE(got_ip.empty());
  EXPECT_EQ(net.ipAddress(), got_ip);
}

// The deadlock fix: connectivity that only appears AFTER start() (e.g. eth0 got its IP a moment
// late, or the setup AP came down) must be reflected without anyone calling connectWifi(). The
// monitor does this by re-polling; we drive one poll deterministically via refresh().
TEST(NetworkManager, RefreshPicksUpConnectivityThatAppearsAfterBoot) {
  EventBus bus;
  std::atomic<int> connected{0};
  std::string got_mode, got_ip;
  bus.subscribe(EventType::NetworkConnected, [&](const Event& e) {
    ++connected;
    got_mode = e.data.value("mode", "");
    got_ip = e.data.value("ip", "");
  });

  auto hal = std::make_unique<ControllableHal>();
  ControllableHal* raw = hal.get();
  NetworkManager net(&bus, std::move(hal));

  // Boot while genuinely disconnected — the boot snapshot must NOT claim connectivity.
  ASSERT_TRUE(net.start().ok());
  bus.drain();
  EXPECT_FALSE(net.isConnected());
  EXPECT_EQ(connected.load(), 0);

  // Ethernet appears after boot. A poll must detect it and publish NetworkConnected.
  raw->set(true, "ethernet", "192.168.1.148");
  net.refresh();
  bus.drain();
  EXPECT_TRUE(net.isConnected());
  EXPECT_EQ(net.mode(), "ethernet");
  EXPECT_EQ(net.ipAddress(), "192.168.1.148");
  EXPECT_EQ(connected.load(), 1);
  EXPECT_EQ(got_mode, "ethernet");

  // Polling again with no change RE-publishes NetworkConnected by design: routine monitor polls emit
  // a periodic connectivity event so consumers (the ProvisioningController) can re-assert desired
  // state (e.g. force the setup hotspot back down if it drifted up). So the count increments; the
  // reported mode/ip stay stable.
  net.refresh();
  bus.drain();
  EXPECT_EQ(connected.load(), 2);
  EXPECT_EQ(got_mode, "ethernet");
  EXPECT_EQ(net.ipAddress(), "192.168.1.148");

  ASSERT_TRUE(net.stop().ok());
}

// The mirror case: connectivity dropping out of band is also detected and published once.
TEST(NetworkManager, RefreshDetectsDisconnect) {
  EventBus bus;
  std::atomic<int> disconnected{0};
  bus.subscribe(EventType::NetworkDisconnected, [&](const Event&) { ++disconnected; });

  auto hal = std::make_unique<ControllableHal>();
  ControllableHal* raw = hal.get();
  raw->set(true, "wifi", "10.0.0.9");  // start connected
  NetworkManager net(&bus, std::move(hal));
  ASSERT_TRUE(net.start().ok());
  bus.drain();
  ASSERT_TRUE(net.isConnected());

  raw->set(false, "", "");  // link drops
  net.refresh();
  bus.drain();
  EXPECT_FALSE(net.isConnected());
  EXPECT_EQ(disconnected.load(), 1);

  ASSERT_TRUE(net.stop().ok());
}
