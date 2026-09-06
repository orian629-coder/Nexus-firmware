#include "pairing/ProvisioningController.h"

#include <cstdlib>

#include "logging/Logger.h"

namespace nexus::pairing {

using core::Event;
using core::EventType;
using core::ServiceState;
using core::Status;

namespace {
using Channel = ProvisioningController::Channel;

// Default actuator: start/stop the systemd unit for the given channel. --no-block so we never stall
// the event thread on systemd; failures are non-fatal (logged by systemd/journal).
void systemctlActuate(Channel ch, bool active) {
  const char* verb = active ? "start" : "stop";
  const char* unit =
      (ch == Channel::Ble) ? "nexus-provisioning.service" : "nexus-hotspot.service";
  std::string cmd = "systemctl --no-block " + std::string(verb) + " " + unit;
  (void)std::system(cmd.c_str());
}
}  // namespace

ProvisioningController::ProvisioningController(core::EventBus* bus, Actuator actuator,
                                              InitialProbe initial)
    : bus_(bus),
      actuator_(actuator ? std::move(actuator) : systemctlActuate),
      initial_(std::move(initial)) {}

Status ProvisioningController::start() {
  // React to connectivity changes and drive the two channels by their own rules (see the header):
  //   BLE     — up until the uplink is Wi-Fi (ethernet keeps it on).
  //   HOTSPOT — up only when there is NO uplink at all; ANY connectivity (ethernet or Wi-Fi) brings
  //             it down, because it owns wlan0 in AP mode and would otherwise block wlan0 from ever
  //             joining a real Wi-Fi network (the AP-vs-client conflict).
  // (NetworkManager reports the real uplink mode; the setup AP on wlan0 is excluded from that
  // determination in NmcliNetworkHal, so "connected" here always means a genuine uplink.)
  bus_->subscribe(EventType::NetworkConnected, [this](const Event& e) {
    const std::string mode = e.data.value("mode", "");
    const bool on_wifi = (mode == "wifi");
    applyConnectivity(/*have_any_uplink=*/true, on_wifi,
                      "connected via " + (mode.empty() ? std::string("uplink") : mode));
  });
  bus_->subscribe(EventType::NetworkDisconnected, [this](const Event&) {
    applyConnectivity(/*have_any_uplink=*/false, /*on_wifi=*/false, "network down");
  });

  // Seed from the ACTUAL current connectivity, not an assumption. NetworkManager starts before this
  // controller and already published (and consumed) the boot-time NetworkConnected on the async bus,
  // so subscribing above is not enough — we must probe now. This is what makes a wired speaker drop
  // the hotspot at boot instead of leaving it up (and deadlocking wlan0).
  state_ = ServiceState::Running;
  InitialState init = initial_ ? initial_() : InitialState{};
  applyConnectivity(init.have_uplink, init.on_wifi,
                    init.have_uplink ? (init.on_wifi ? "boot: on wifi" : "boot: wired uplink")
                                     : "boot: no network");
  return Status::success();
}

Status ProvisioningController::stop() {
  setChannel(Channel::Hotspot, false, "service stop");
  setChannel(Channel::Ble, false, "service stop");
  state_ = ServiceState::Stopped;
  return Status::success();
}

void ProvisioningController::applyConnectivity(bool have_any_uplink, bool on_wifi,
                                               const std::string& reason) {
  // BLE stays up until we're actually on Wi-Fi; the hotspot must yield wlan0 the moment ANY uplink
  // exists so a client association can succeed.
  setChannel(Channel::Ble, /*on=*/!on_wifi, reason);
  // The hotspot must be DOWN whenever an uplink exists. Re-assert it every time (not just on a state
  // change) so that if the AP ever drifts up out of band — a stray `systemctl start`, a race, a
  // manual test — the next connectivity poll forces it back down instead of leaving wlan0 pinned in
  // AP mode. `systemctl stop` is idempotent, so re-asserting an already-down hotspot is a no-op.
  const bool hotspot_desired = !have_any_uplink;
  setChannel(Channel::Hotspot, hotspot_desired, reason, /*enforce=*/hotspot_desired == false);
}

void ProvisioningController::setChannel(Channel ch, bool on, const std::string& reason,
                                        bool enforce) {
  bool& flag = (ch == Channel::Ble) ? ble_on_ : hotspot_on_;
  const bool changed = (on != flag);
  if (!changed && !enforce) return;  // idempotent: nothing to do unless we're force-re-asserting
  flag = on;
  const char* label = (ch == Channel::Ble) ? "BLE provisioning" : "Wi-Fi setup hotspot";
  if (changed) {
    // Only log real transitions; a periodic re-assertion that finds nothing to fix stays silent to
    // avoid flooding the journal every poll interval.
    NX_LOG_INFO("provisioning",
                std::string(on ? "starting " : "stopping ") + label + " (" + reason + ")");
  }
  if (actuator_) actuator_(ch, on);
}

}  // namespace nexus::pairing
