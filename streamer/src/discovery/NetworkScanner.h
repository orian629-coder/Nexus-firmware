#pragma once

#include <string>
#include <vector>

namespace nexus::streamer::discovery {

// A nexus speaker found by scanning the LAN (probing each host's REST API), independent of mDNS.
// This is the "Scan Network" installer flow from the AOA reference: sweep the subnet, ask each IP
// GET /api/status, and keep the ones that answer like a nexus speaker.
struct ScannedSpeaker {
  std::string ip;
  std::string device_id;
  std::string state;        // e.g. SETUP_MODE / OFFLINE / ONLINE
  std::string software_version;
  bool paired = false;
  bool setup_mode = false;
  std::string box_public_key;  // present when advertised in status; enables pairing
};

// Scan an IPv4 /24 subnet derived from `cidr_or_ip` (e.g. "192.168.1.0/24" or "192.168.1.148" ->
// 192.168.1.1..254). Probes http://<ip>:<port>/api/status on each host concurrently with a short
// timeout, parses the JSON, and returns every host that responds like a nexus speaker (has a
// device_id). Blocking; typically completes in a couple of seconds for a /24.
std::vector<ScannedSpeaker> scanSubnet(const std::string& cidr_or_ip, int port = 8080,
                                       int timeout_ms = 400, int concurrency = 64);

// Best-effort detect this host's own IPv4 (the outbound one) and return its /24, e.g.
// "192.168.1.0/24". Returns `fallback` if it can't be determined.
std::string localSubnetOr(const std::string& fallback);

}  // namespace nexus::streamer::discovery
