#pragma once

#include <optional>
#include <string>

// The private-AP naming + passphrase contract for the streamer's "Nexus-<streamer_id>" network.
// The STREAMER (which hosts the AP) and every SPEAKER (which joins it) derive these identically
// from the streamer_id, so both sides MUST agree byte-for-byte. Treat this like the mDNS wire
// contract: changing the salt, length, or SSID shape breaks how speakers join.
namespace nexus::identity {

struct ApCredentials {
  std::string ssid;        // "Nexus-<streamer_id>", e.g. "Nexus-STR-a14ad83e"
  std::string passphrase;  // deterministic WPA2 passphrase derived from streamer_id
};

// SSID prefix that marks a Nexus private AP.
inline constexpr char kApSsidPrefix[] = "Nexus-";

// Derive the AP SSID + WPA2 passphrase for a streamer id (e.g. "STR-a14ad83e").
ApCredentials deriveApCredentials(const std::string& streamer_id);

// If `ssid` is a Nexus private-AP SSID ("Nexus-STR-..."), return the embedded streamer_id;
// otherwise std::nullopt. The setup AP ("Nexus-Setup") and foreign SSIDs return nullopt.
std::optional<std::string> streamerIdFromApSsid(const std::string& ssid);

// True iff id is exactly "STR-" followed by 8 lowercase hex chars.
bool isWellFormedStreamerId(const std::string& id);

}  // namespace nexus::identity
