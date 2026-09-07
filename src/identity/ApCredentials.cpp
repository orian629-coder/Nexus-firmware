#include "identity/ApCredentials.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "identity/Crypto.h"

namespace nexus::identity {
namespace {
// Fixed salt versions the derivation. Bump the suffix only with a coordinated streamer+speaker
// rollout — it changes every AP passphrase in the fleet.
constexpr char kPassphraseSalt[] = "nexus-audio-ap-v1:";
// 24 lowercase-hex chars: comfortably inside WPA2's 8-63 printable-ASCII range.
constexpr std::size_t kPassphraseLen = 24;
constexpr char kStreamerIdPrefix[] = "STR-";
}  // namespace

ApCredentials deriveApCredentials(const std::string& streamer_id) {
  const std::string salted = std::string(kPassphraseSalt) + streamer_id;
  const std::vector<std::uint8_t> bytes(salted.begin(), salted.end());
  const std::string hex = crypto::sha256Hex(bytes);  // 64 lowercase hex chars

  ApCredentials creds;
  creds.ssid = std::string(kApSsidPrefix) + streamer_id;
  creds.passphrase = hex.substr(0, kPassphraseLen);
  return creds;
}

std::optional<std::string> streamerIdFromApSsid(const std::string& ssid) {
  const std::string prefix(kApSsidPrefix);
  if (ssid.rfind(prefix, 0) != 0) return std::nullopt;
  std::string id = ssid.substr(prefix.size());
  if (id.rfind(kStreamerIdPrefix, 0) != 0) return std::nullopt;  // excludes "Nexus-Setup"
  return id;
}

}  // namespace nexus::identity
