#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/EventBus.h"
#include "core/IService.h"
#include "network/INetworkHal.h"

namespace nexus::network {

// Manages network connectivity through an INetworkHal. Connecting/disconnecting emits
// NetworkConnected / NetworkDisconnected / IpChanged events that SystemManager maps to state
// transitions. The OS-specific work (nmcli / Wi-Fi) lives entirely behind the HAL, so this
// module's logic and event wiring are fully testable off-target with StubNetworkHal.
//
// A background monitor re-polls the HAL every few seconds so connectivity that changes OUT OF BAND
// — an Ethernet cable plugged in, DHCP finally granting an IP after boot, the setup AP coming down
// — is reflected without anyone calling connectWifi(). Without this the cached state, seeded once at
// start(), could get stuck (e.g. "disconnected" forever if start() ran before eth0 had an IP),
// which in turn kept the provisioning hotspot up and deadlocked wlan0.
class NetworkManager : public core::IService {
 public:
  // Takes ownership of the HAL. If null, a HAL is selected at construction based on NEXUS_STUB_HAL.
  explicit NetworkManager(core::EventBus* bus, std::unique_ptr<INetworkHal> hal = nullptr);
  ~NetworkManager() override;

  std::string name() const override { return "network"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }
  core::Status healthCheck() override;

  // Connect to Wi-Fi and, on success, publish NetworkConnected. Credentials come from the pairing
  // flow via SecureStorage — never from config. The params overload carries the advanced setup
  // knobs (hidden SSID, band, static IP); the ssid+psk overload is the common case.
  core::Status connectWifi(const std::string& ssid, const std::string& psk);
  core::Status connectWifi(const WifiConnectParams& params);

  core::Result<std::vector<WifiNetwork>> scan();
  bool isConnected() const;
  std::string ipAddress() const;
  // Current uplink mode ("wifi" | "ethernet" | ""). Lets a service that starts AFTER network query
  // the connectivity it may have missed the boot-time NetworkConnected event for (the bus is async
  // and one-shot at boot). Empty when disconnected.
  std::string mode() const;

  // Poll the HAL once and publish any connect/disconnect/IP delta. Exposed for tests; also the unit
  // of work the monitor thread repeats. Thread-safe.
  void refresh();

 private:
  // Apply a freshly polled status. `periodic` marks a routine monitor re-poll: on no change it still
  // re-publishes NetworkConnected (so consumers can re-assert desired state) but doesn't re-log.
  void applyStatus(const NetworkStatus& st, bool periodic = false);
  void publishConnected(const NetworkStatus& st, bool periodic = false);
  // Edge-triggered by default (only a real connected→disconnected transition is published, so the
  // periodic poll doesn't spam the bus). `force` publishes even with no state change — required on
  // the failed-connect path, which has already torn the setup hotspot down and must trigger its
  // recovery. Caller must hold mutex_.
  void publishDisconnected(bool force = false);
  void monitorLoop();

  core::EventBus* bus_;
  std::unique_ptr<INetworkHal> hal_;
  core::ServiceState state_ = core::ServiceState::Stopped;

  mutable std::mutex mutex_;  // guards connected_/ip_/mode_ (monitor writes, callers read)
  bool connected_ = false;
  std::string ip_;
  std::string mode_;  // last known uplink mode; mirrors the NetworkConnected "mode" field

  // Background connectivity monitor.
  std::thread monitor_;
  std::atomic<bool> monitor_running_{false};
  std::condition_variable monitor_cv_;
  std::mutex monitor_mutex_;
};

}  // namespace nexus::network
