#include "main/ApCredentialsCli.h"

#include <exception>
#include <fstream>

#include <nlohmann/json.hpp>

#include "config/ConfigSerialization.h"  // from_json(PairingConfig)
#include "config/SpeakerConfig.h"
#include "identity/ApCredentials.h"

namespace nexus::app {

// A streamer_id is trusted only if it matches the streamer's own generation shape: "STR-" followed
// by exactly 8 lowercase hex chars (StreamerIdentity: "STR-" + sha256Hex(pubkey)[:8]). The value in
// the speaker config is peer-supplied at pairing time, so we fail closed here: anything else yields
// NO credentials, keeping shell consumers (speaker-ap-join.sh) from ever seeing an SSID that could
// carry shell/regex metacharacters. This is the single trusted producer for those consumers.
// (Shape enforced via nexus::identity::isWellFormedStreamerId, the shared validator.)

std::optional<nexus::identity::ApCredentials> apCredentialsFromConfig(
    const std::string& config_path) {
  try {
    std::ifstream in(config_path);
    if (!in) return std::nullopt;

    nlohmann::json j;
    in >> j;
    if (!j.contains("pairing")) return std::nullopt;

    const auto pairing = j.at("pairing").get<nexus::config::PairingConfig>();
    if (!pairing.paired || !nexus::identity::isWellFormedStreamerId(pairing.streamer_id))
      return std::nullopt;

    return nexus::identity::deriveApCredentials(pairing.streamer_id);
  } catch (const std::exception&) {
    // Unreadable or malformed config -> no credentials; caller lets normal onboarding proceed.
    return std::nullopt;
  }
}

}  // namespace nexus::app
