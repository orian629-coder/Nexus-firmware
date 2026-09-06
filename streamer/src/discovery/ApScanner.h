#pragma once

#include <string>
#include <vector>

namespace nexus::streamer::discovery {

// A speaker's setup access point, seen over the air.
//
// This is the ONLY way to find a speaker that has never been on a network: mDNS and the subnet
// sweep both require the device to already hold credentials, so a factory-fresh speaker is
// invisible to them. It raises "Nexus-Setup" instead, and the streamer's own Wi-Fi radio can see
// that AP without joining it.
//
// The streamer host is expected to have TWO interfaces — a wired uplink that carries the LAN, and
// a Wi-Fi radio kept free for exactly this. Scanning does not disturb the uplink.
struct ScannedAp {
  std::string ssid;
  std::string bssid;       // the AP's MAC — the only hardware id visible before we associate
  int signal = 0;          // nmcli SIGNAL, 0..100
  bool secured = false;    // has any security (the setup AP is normally open)
};

// Scan `iface` for nearby access points whose SSID starts with `ssid_prefix` (default the setup AP
// name the speaker raises). Blocking; a scan takes a couple of seconds.
//
// Implemented over `nmcli device wifi list` rather than a raw netlink scan: NetworkManager already
// owns the radio on this platform, and asking it avoids fighting it for the interface.
std::vector<ScannedAp> scanAccessPoints(const std::string& iface = "wlan0",
                                        const std::string& ssid_prefix = "Nexus-",
                                        bool rescan = true);

// Look up the MAC address for an IPv4 address in the local ARP/neighbour table. Returns "" when it
// is not known.
//
// This is a DISPLAY aid, never an identity. Three reasons it cannot be a key:
//   • it only resolves for hosts on the same L2 segment — across a VLAN or router it returns "";
//   • the entry may simply not be cached yet, so "" does not mean "no such device";
//   • a speaker has a different MAC per interface, so the same device reads differently on
//     Ethernet and Wi-Fi.
// The stable identity is device_id, which the device itself reports.
std::string macForIp(const std::string& ip);

}  // namespace nexus::streamer::discovery
