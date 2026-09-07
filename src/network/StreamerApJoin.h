#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::network {

class NetworkManager;

// Look for the paired streamer's private AP ("Nexus-<streamer_id>") among the current WiFi scan
// results and, if present and we're not already connected, join it with the derived passphrase.
// Best-effort: NotFound (AP not in range) is a normal, non-fatal outcome the caller ignores.
//   - Ok           already connected, or the join was issued successfully
//   - InvalidArg   streamer_id is empty (speaker not paired)
//   - NotFound     the streamer AP is not in range
//   - (propagated) a scan or connect failure from the network HAL
core::Status joinStreamerAp(NetworkManager& net, const std::string& streamer_id);

}  // namespace nexus::network
