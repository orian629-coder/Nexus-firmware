#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/EventBus.h"
#include "identity/ApCredentials.h"
#include "network/INetworkHal.h"
#include "network/NetworkManager.h"
#include "network/StreamerApJoin.h"

using namespace nexus;
using nexus::network::INetworkHal;
using nexus::network::NetworkManager;
using nexus::network::NetworkStatus;
using nexus::network::WifiConnectParams;
using nexus::network::WifiNetwork;

namespace {
// A network HAL with a settable scan list + connectivity that records the SSID/psk of the last
// connect — enough to prove joinStreamerAp derives creds and joins the right AP.
class FakeHal : public INetworkHal {
 public:
  using INetworkHal::connectWifi;  // keep the ssid+psk convenience overload

  void setScan(std::vector<WifiNetwork> nets) {
    std::lock_guard<std::mutex> l(m_);
    scan_ = std::move(nets);
  }
  void setConnected(bool c) { std::lock_guard<std::mutex> l(m_); connected_ = c; }
  std::string lastSsid() { std::lock_guard<std::mutex> l(m_); return last_ssid_; }
  std::string lastPsk() { std::lock_guard<std::mutex> l(m_); return last_psk_; }
  int connectCalls() { std::lock_guard<std::mutex> l(m_); return connect_calls_; }

  core::Result<std::vector<WifiNetwork>> scanWifi() override {
    std::lock_guard<std::mutex> l(m_);
    return scan_;
  }
  core::Status connectWifi(const WifiConnectParams& params) override {
    std::lock_guard<std::mutex> l(m_);
    ++connect_calls_;
    last_ssid_ = params.ssid;
    last_psk_ = params.psk;
    connected_ = true;
    return core::Status::success();
  }
  core::Status disconnect() override {
    std::lock_guard<std::mutex> l(m_);
    connected_ = false;
    return core::Status::success();
  }
  core::Result<NetworkStatus> status() override {
    std::lock_guard<std::mutex> l(m_);
    NetworkStatus s;
    s.connected = connected_;
    s.mode = connected_ ? "wifi" : "";
    return s;
  }

 private:
  std::mutex m_;
  std::vector<WifiNetwork> scan_;
  bool connected_ = false;
  int connect_calls_ = 0;
  std::string last_ssid_;
  std::string last_psk_;
};
}  // namespace

TEST(JoinStreamerAp, JoinsTheApWhenInRange) {
  core::EventBus bus;
  auto hal = std::make_unique<FakeHal>();
  auto* raw = hal.get();
  const auto creds = identity::deriveApCredentials("STR-LAB01");
  raw->setScan({{creds.ssid, -40}, {"Handsome", -55}});
  NetworkManager net(&bus, std::move(hal));

  const core::Status st = network::joinStreamerAp(net, "STR-LAB01");

  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(raw->connectCalls(), 1);
  EXPECT_EQ(raw->lastSsid(), creds.ssid);
  EXPECT_EQ(raw->lastPsk(), creds.passphrase);
}

TEST(JoinStreamerAp, ReturnsNotFoundWhenApAbsent) {
  core::EventBus bus;
  auto hal = std::make_unique<FakeHal>();
  auto* raw = hal.get();
  raw->setScan({{"Handsome", -55}, {"Guest", -70}});
  NetworkManager net(&bus, std::move(hal));

  const core::Status st = network::joinStreamerAp(net, "STR-LAB01");

  EXPECT_EQ(st.code(), core::ErrorCode::NotFound);
  EXPECT_EQ(raw->connectCalls(), 0);
}

TEST(JoinStreamerAp, RejectsEmptyStreamerId) {
  core::EventBus bus;
  NetworkManager net(&bus, std::make_unique<FakeHal>());
  EXPECT_EQ(network::joinStreamerAp(net, "").code(), core::ErrorCode::InvalidArg);
}

TEST(JoinStreamerAp, NoOpWhenAlreadyConnected) {
  core::EventBus bus;
  auto hal = std::make_unique<FakeHal>();
  auto* raw = hal.get();
  raw->setConnected(true);
  NetworkManager net(&bus, std::move(hal));
  net.refresh();  // pull HAL status into the manager's cache so isConnected() is true

  const core::Status st = network::joinStreamerAp(net, "STR-LAB01");

  EXPECT_TRUE(st.ok());
  EXPECT_EQ(raw->connectCalls(), 0);
}
