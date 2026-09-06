#include "network/NetworkManager.h"

#include <chrono>

#include "logging/Logger.h"

#if NEXUS_STUB_HAL
// StubNetworkHal is defined in the interface header.
#else
#include "network/NmcliNetworkHal.h"
#endif

namespace nexus::network {

using core::ErrorCode;
using core::Result;
using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<INetworkHal> makeDefaultHal() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubNetworkHal>();
#else
  return std::make_unique<NmcliNetworkHal>();
#endif
}

// How often the background monitor re-polls the HAL for out-of-band connectivity changes.
constexpr std::chrono::seconds kMonitorInterval{5};
}  // namespace

NetworkManager::NetworkManager(core::EventBus* bus, std::unique_ptr<INetworkHal> hal)
    : bus_(bus), hal_(hal ? std::move(hal) : makeDefaultHal()) {}

NetworkManager::~NetworkManager() { stop(); }

Status NetworkManager::start() {
  state_ = ServiceState::Running;
  // Reflect any already-present connectivity (e.g. Ethernet) at boot.
  refresh();
  // Then keep watching: connectivity can change out of band (cable in/out, DHCP granting an IP a
  // moment after boot, the setup AP coming down). Re-polling keeps the cached state — and every
  // consumer of it (provisioning hotspot gating, status API) — from getting stuck on a stale
  // boot-time snapshot.
  monitor_running_ = true;
  monitor_ = std::thread([this] { monitorLoop(); });
  return Status::success();
}

Status NetworkManager::stop() {
  // Signal the monitor and join it before tearing down. Take monitor_mutex_ around the flag flip so
  // the notify can't slip between the loop's predicate check and its wait_for (a missed wakeup would
  // stall shutdown for a full interval).
  if (monitor_running_.load()) {
    {
      std::lock_guard<std::mutex> lk(monitor_mutex_);
      monitor_running_ = false;
    }
    monitor_cv_.notify_all();
    if (monitor_.joinable()) monitor_.join();
  }
  state_ = ServiceState::Stopped;
  return Status::success();
}

Status NetworkManager::healthCheck() {
  return state_ == ServiceState::Running ? Status::success()
                                         : Status::error(ErrorCode::Unknown, "network not running");
}

Result<std::vector<WifiNetwork>> NetworkManager::scan() { return hal_->scanWifi(); }

Status NetworkManager::connectWifi(const std::string& ssid, const std::string& psk) {
  return connectWifi(WifiConnectParams{ssid, psk, false, "", "", "", ""});
}

Status NetworkManager::connectWifi(const WifiConnectParams& params) {
  NX_LOG_INFO("network", "connecting to wifi ssid=" + params.ssid +
                             (params.hidden ? " (hidden)" : "") +
                             (params.band.empty() ? "" : " band=" + params.band) +
                             (params.static_ip.empty() ? "" : " static=" + params.static_ip));
  Status s = hal_->connectWifi(params);
  if (!s.ok()) {
    NX_LOG_ERROR("network", s.code(), "wifi connect failed: " + s.message());
    // force: the attempt already brought the setup hotspot down, so recovery must be re-asserted
    // even though we were never connected in the first place.
    { std::lock_guard<std::mutex> lk(mutex_); publishDisconnected(/*force=*/true); }
    return s;
  }
  auto st = hal_->status();
  if (st.ok() && st.value().connected) {
    applyStatus(st.value());  // takes mutex_, publishes NetworkConnected on the delta
    return Status::success();
  }
  { std::lock_guard<std::mutex> lk(mutex_); publishDisconnected(/*force=*/true); }
  return Status::error(ErrorCode::Timeout, "connect command succeeded but no IP");
}

bool NetworkManager::isConnected() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return connected_;
}

std::string NetworkManager::ipAddress() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return ip_;
}

std::string NetworkManager::mode() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return mode_;
}

void NetworkManager::monitorLoop() {
  std::unique_lock<std::mutex> lk(monitor_mutex_);
  while (monitor_running_) {
    // Wait up to the interval, but wake immediately on stop().
    monitor_cv_.wait_for(lk, kMonitorInterval, [this] { return !monitor_running_; });
    if (!monitor_running_) break;
    lk.unlock();
    refresh();  // poll the HAL outside the monitor lock (refresh takes mutex_)
    lk.lock();
  }
}

void NetworkManager::refresh() {
  auto st = hal_->status();
  if (!st.ok()) return;  // transient HAL error: keep last known state, try again next tick
  // periodic=true: this is a routine monitor poll. Re-publish NetworkConnected even when nothing
  // changed so downstream consumers (the ProvisioningController) can re-assert their desired state —
  // e.g. force the setup hotspot back down if it drifted up while an uplink exists. The event is
  // idempotent for everyone else, and we still log only on real transitions.
  applyStatus(st.value(), /*periodic=*/true);
}

void NetworkManager::applyStatus(const NetworkStatus& st, bool periodic) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (st.connected) {
    publishConnected(st, periodic);
  } else {
    publishDisconnected();
  }
}

// NOTE: callers (applyStatus, connectWifi) hold mutex_. A change is always published; a `periodic`
// re-poll with no change re-publishes NetworkConnected (for re-assertion downstream) but stays
// silent in the log so the journal isn't flooded every poll tick.
void NetworkManager::publishConnected(const NetworkStatus& st, bool periodic) {
  const bool was_connected = connected_;
  const bool ip_changed = was_connected && st.ip_address != ip_;
  const bool mode_changed = was_connected && st.mode != mode_;
  const bool changed = !was_connected || ip_changed || mode_changed;
  if (!changed && !periodic) return;  // no change and not a periodic re-assert → nothing to do
  connected_ = true;
  ip_ = st.ip_address;
  mode_ = st.mode;
  if (changed) {
    NX_LOG_INFO("network", "connected mode=" + st.mode + " ip=" + st.ip_address);
  }
  if (bus_) {
    bus_->publish(core::Event{core::EventType::NetworkConnected, "network",
                              {{"mode", st.mode}, {"ip", st.ip_address}}});
    if (ip_changed) {
      bus_->publish(core::Event{core::EventType::IpChanged, "network", {{"ip", st.ip_address}}});
    }
  }
}

void NetworkManager::publishDisconnected(bool force) {
  // `force` exists for the failed-connect path. Normally this is edge-triggered: only a real
  // connected→disconnected transition is announced, so a periodic status poll doesn't spam the bus.
  // But a Wi-Fi join that fails during onboarding starts from connected_ == false (there was never
  // an uplink), so the edge never occurs and nothing is published — while connectWifi() has already
  // torn down the setup hotspot to free the radio. The result was a device with no Wi-Fi and no
  // setup AP, unrecoverable without physical access. On that path the event must be re-asserted
  // even with no state change, so ProvisioningController brings the hotspot back.
  const bool was_connected = connected_;
  connected_ = false;
  ip_.clear();
  mode_.clear();
  if (!was_connected && !force) return;
  if (was_connected) NX_LOG_WARN("network", "network disconnected");
  if (bus_) bus_->publish(core::Event{core::EventType::NetworkDisconnected, "network"});
}

}  // namespace nexus::network
