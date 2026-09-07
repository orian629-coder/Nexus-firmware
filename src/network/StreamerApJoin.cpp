#include "network/StreamerApJoin.h"

#include "identity/ApCredentials.h"
#include "network/NetworkManager.h"

namespace nexus::network {

core::Status joinStreamerAp(NetworkManager& net, const std::string& streamer_id) {
  if (streamer_id.empty()) {
    return core::Status::error(core::ErrorCode::InvalidArg, "no paired streamer_id");
  }
  if (net.isConnected()) {
    return core::Status::success();  // already on a network; the monitor reports it
  }

  const identity::ApCredentials creds = identity::deriveApCredentials(streamer_id);

  auto scan = net.scan();
  if (!scan.ok()) return scan.status();

  bool in_range = false;
  for (const auto& n : scan.value()) {
    if (n.ssid == creds.ssid) {
      in_range = true;
      break;
    }
  }
  if (!in_range) {
    return core::Status::error(core::ErrorCode::NotFound,
                               "streamer AP not in range: " + creds.ssid);
  }
  return net.connectWifi(creds.ssid, creds.passphrase);
}

}  // namespace nexus::network
