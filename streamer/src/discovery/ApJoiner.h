#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::streamer::discovery {

// Temporarily associates the streamer's Wi-Fi radio with a speaker's setup AP so the pairing
// handshake can reach a device that is not yet on any network.
//
// WHY THIS IS SAFE HERE: the streamer host has a wired uplink carrying the LAN and a separate Wi-Fi
// radio reserved for onboarding. Joining a setup AP therefore costs nothing — the control UI, the
// other speakers, and SSH all stay up on Ethernet throughout. On a single-radio host (a laptop)
// the same operation would drop the machine off the network mid-flow, which is precisely why
// onboarding lives on the streamer and not in the operator's browser.
//
// The join is scoped: leave() tears the temporary profile down again, so the radio is returned to
// its previous state whether the pairing succeeded or failed.
class ApJoiner {
 public:
  explicit ApJoiner(std::string iface = "wlan0") : iface_(std::move(iface)) {}

  // Associate with `ssid` (open network — the speaker's setup AP has no password by default).
  // Blocking; returns once the interface reports a connection or the attempt fails.
  core::Status join(const std::string& ssid, int timeout_s = 30);

  // Tear down the temporary profile and let NetworkManager restore whatever it had before.
  // Safe to call when not joined.
  core::Status leave();

  // The gateway address on the joined AP — the speaker itself. Empty if not joined.
  // The speaker's hotspot always answers on a fixed address, but this reads the real one rather
  // than assuming, so a changed hotspot subnet does not silently break pairing.
  std::string gateway() const;

  const std::string& iface() const { return iface_; }

 private:
  std::string iface_;
  std::string profile_;  // name of the temporary connection profile, empty when not joined
};

}  // namespace nexus::streamer::discovery
