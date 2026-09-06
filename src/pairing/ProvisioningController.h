#pragma once

#include <functional>
#include <string>

#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::pairing {

// Turns the onboarding channels on/off based on connectivity. There are TWO independent channels,
// gated by DIFFERENT rules because they consume different radios:
//
//   * BLE      — a phone connects (Just Works, no code) to hand over Wi-Fi + streamer creds. BLE
//                does NOT touch wlan0, so it is harmless to keep advertising until the speaker is on
//                Wi-Fi (the product rule: "advertise setup until on Wi-Fi"). Ethernet keeps it on.
//
//   * HOTSPOT  — the Wi-Fi setup AP ("Nexus-Setup") on wlan0 + captive portal, for iOS which can't
//                do Web Bluetooth. It OWNS wlan0 in AP mode. A single Wi-Fi radio cannot be an AP
//                and a client at once, so keeping the AP up while wlan0 needs to JOIN a network
//                deadlocks onboarding: the join fails and tears the AP down mid-handshake. Therefore
//                the hotspot must be gated on "no connectivity AT ALL" — it comes down the moment
//                the speaker has ANY uplink (Ethernet OR Wi-Fi), freeing wlan0 to associate. This is
//                the fix for the AP-vs-client conflict that blocked Wi-Fi joins on a wired speaker.
//
// The channels' GATT/AP servers live in separate helpers (provisioning/ble_provisioning.py as
// nexus-provisioning.service; scripts/hotspot.sh as nexus-hotspot.service). This controller only
// decides *when* each runs, by reacting to NetworkConnected / NetworkDisconnected events. The
// actuator is injected so it's testable off-target (the default shells out to systemctl on the Pi).
class ProvisioningController : public core::IService {
 public:
  // The two onboarding channels, actuated independently (they gate on different connectivity rules).
  enum class Channel { Ble, Hotspot };

  // `actuate(channel, true)` must start that channel's service; `false` must stop it. Defaults to
  // driving `systemctl --no-block start/stop` of nexus-provisioning.service (BLE) /
  // nexus-hotspot.service (hotspot) on the device.
  using Actuator = std::function<void(Channel, bool /*active*/)>;

  // Connectivity already present when the controller starts. The boot-time NetworkConnected event
  // is published by NetworkManager BEFORE this controller subscribes (network starts first, and the
  // bus is async + one-shot at boot), so we must PROBE the current state at start() rather than
  // assume "no network". Without this, a wired speaker would leave the hotspot up forever — the very
  // AP-vs-client deadlock this controller exists to avoid.
  struct InitialState {
    bool have_uplink = false;  // any uplink (ethernet or wifi)
    bool on_wifi = false;      // the uplink is Wi-Fi
  };
  using InitialProbe = std::function<InitialState()>;

  // `initial` (optional) is queried once at start() to seed connectivity; if null, start() assumes
  // no network (both channels on) and waits for events.
  explicit ProvisioningController(core::EventBus* bus, Actuator actuator = nullptr,
                                  InitialProbe initial = nullptr);

  std::string name() const override { return "provisioning"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // BLE advertising state (kept as the primary "advertising" signal for callers/tests).
  bool advertising() const { return ble_on_; }
  // Whether the Wi-Fi setup hotspot (AP on wlan0) is currently up.
  bool hotspotOn() const { return hotspot_on_; }

 private:
  // Apply a channel's desired state. Normally idempotent (actuates only on a real change), but with
  // enforce=true it re-issues the actuation even when the state matches — used to force the hotspot
  // back to its desired-DOWN state if it drifted up out of band. Logs only on real transitions.
  void setChannel(Channel ch, bool on, const std::string& reason, bool enforce = false);
  // Recompute both channels from the current connectivity and actuate any changes.
  void applyConnectivity(bool have_any_uplink, bool on_wifi, const std::string& reason);

  core::EventBus* bus_;
  Actuator actuator_;
  InitialProbe initial_;
  core::ServiceState state_ = core::ServiceState::Stopped;
  bool ble_on_ = false;      // BLE: up until Wi-Fi is the uplink
  bool hotspot_on_ = false;  // Hotspot (wlan0 AP): up only when there is NO uplink at all
};

}  // namespace nexus::pairing
