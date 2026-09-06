#pragma once

#include <optional>

namespace nexus::streamer::net {

// Reads this host's own Wi-Fi link quality — the RSSI of the streamer's connection to its access
// point. This is the "streamer signal" the speaker's kiosk displays: the streamer reports it to
// each paired speaker over the control channel, because the speaker cannot measure a remote peer's
// radio itself.
//
// On Linux the value comes from /proc/net/wireless (the same source `iwconfig`/`iw` read), so no
// external tools or root are needed. On a wired streamer (no Wi-Fi interface) there is nothing to
// report and readRssiDbm() returns nullopt — the speaker then falls back to its own RTT link meter.
struct WifiSignal {
  // Current Wi-Fi signal in dBm (negative; e.g. -45 strong, -80 weak), or nullopt if this host has
  // no associated Wi-Fi interface (wired, or radio down). Reads the first wireless interface with a
  // live link. Never throws.
  static std::optional<int> readRssiDbm();
};

}  // namespace nexus::streamer::net
