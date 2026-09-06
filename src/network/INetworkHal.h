#pragma once

#include <string>
#include <vector>

#include "core/Result.h"

namespace nexus::network {

struct WifiNetwork {
  std::string ssid;
  int signal_dbm = 0;
};

struct NetworkStatus {
  bool connected = false;
  std::string mode;        // "wifi" | "ethernet" | ""
  std::string ip_address;  // empty when disconnected
  int wifi_signal_dbm = 0;
};

// Everything needed to join one Wi-Fi network from the setup page. Only `ssid` is required; the rest
// are optional refinements the advanced setup form can send. Kept in one struct so adding a knob
// later doesn't churn the connectWifi signature across the HAL, NetworkManager, and callers.
struct WifiConnectParams {
  std::string ssid;
  std::string psk;
  // Hidden network: the AP doesn't broadcast its SSID, so nmcli must be told to actively probe for
  // it (hidden yes) or the join silently finds nothing.
  bool hidden = false;
  // Preferred band: "" = auto (let nmcli pick), "bg" = 2.4GHz, "a" = 5GHz. Useful when the same SSID
  // exists on both bands and one is flaky (e.g. WPA3-on-5GHz association issues on some radios).
  std::string band;
  // Static IPv4 (advanced). When ip is non-empty, configure a manual address instead of DHCP:
  //   ip is "A.B.C.D/prefix" (e.g. "192.168.1.50/24"); gateway/dns optional.
  std::string static_ip;
  std::string gateway;
  std::string dns;
};

// Hardware/OS abstraction for network operations. On the Pi this is backed by NetworkManager
// (nmcli); on dev hosts a stub simulates connectivity so NetworkManager's logic and event wiring
// are fully testable off-target. Selected at build time by NEXUS_STUB_HAL.
class INetworkHal {
 public:
  virtual ~INetworkHal() = default;

  virtual core::Result<std::vector<WifiNetwork>> scanWifi() = 0;
  // Join a Wi-Fi network. The two-arg overload is the common ssid+psk case; the params overload
  // carries the advanced setup knobs (hidden, band, static IP).
  virtual core::Status connectWifi(const std::string& ssid, const std::string& psk) {
    return connectWifi(WifiConnectParams{ssid, psk, false, "", "", "", ""});
  }
  virtual core::Status connectWifi(const WifiConnectParams& params) = 0;
  virtual core::Status disconnect() = 0;
  virtual core::Result<NetworkStatus> status() = 0;
};

// Simulated network for development hosts. Starts disconnected; connectWifi() succeeds and marks
// it connected with a synthetic IP so higher layers can be exercised.
class StubNetworkHal : public INetworkHal {
 public:
  core::Result<std::vector<WifiNetwork>> scanWifi() override {
    return std::vector<WifiNetwork>{{"NexusLab", -45}, {"Guest", -70}};
  }
  using INetworkHal::connectWifi;  // keep the ssid+psk convenience overload visible
  core::Status connectWifi(const WifiConnectParams& params) override {
    status_.connected = true;
    status_.mode = "wifi";
    // Honor a static IP if given, else a synthetic DHCP-style address.
    status_.ip_address = params.static_ip.empty()
                             ? std::string("10.0.0.50")
                             : params.static_ip.substr(0, params.static_ip.find('/'));
    status_.wifi_signal_dbm = -45;
    last_params_ = params;
    return core::Status::success();
  }
  core::Status disconnect() override {
    status_ = NetworkStatus{};
    return core::Status::success();
  }
  core::Result<NetworkStatus> status() override { return status_; }

  // Exposed for tests: the params the last connect was asked to use.
  const WifiConnectParams& lastParams() const { return last_params_; }

 private:
  NetworkStatus status_;
  WifiConnectParams last_params_;
};

}  // namespace nexus::network
