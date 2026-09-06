#pragma once

#include <cstdint>
#include <string>

namespace nexus::pairing {

// Wi-Fi credentials carried inside the sealed box (never logged, never stored in config).
struct WifiCredentials {
  std::string ssid;
  std::string psk;
};

// The pairing request the Streamer sends to the speaker's control endpoint during setup. The
// Streamer proves possession of its advertised Ed25519 key by signing the canonical request bytes.
// Wi-Fi credentials are sealed (crypto_box_seal) to the speaker's advertised X25519 public key so
// only this speaker can decrypt them.
struct PairingRequest {
  std::string streamer_id;
  std::string streamer_public_key;   // base64 Ed25519 — must match the beacon/mDNS record
  std::string site_id;
  std::string initial_speaker_name;
  std::string setup_code;            // time-limited code shown to the user
  std::string sealed_wifi_b64;       // sealed box: JSON {ssid,psk} encrypted to speaker X25519 key
  std::string signature_b64;         // Ed25519 signature over canonicalBytes()

  // Canonical byte string the signature is computed over (order fixed; excludes signature).
  std::string canonicalString() const {
    return streamer_id + "\n" + streamer_public_key + "\n" + site_id + "\n" +
           initial_speaker_name + "\n" + setup_code + "\n" + sealed_wifi_b64;
  }
};

// Result of a completed pairing (what gets persisted / acted on).
struct PairingResult {
  std::string streamer_id;
  std::string streamer_public_key;
  std::string site_id;
  std::string speaker_name;
  WifiCredentials wifi;  // consumed by NetworkManager, then only the PSK lives in SecureStorage
};

}  // namespace nexus::pairing
