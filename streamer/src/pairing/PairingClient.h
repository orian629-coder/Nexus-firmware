#pragma once

#include <string>

#include "control/ILineTransport.h"
#include "core/Result.h"

namespace nexus::streamer::pairing {

// Everything the streamer needs to pair with one speaker during setup.
struct PairingParams {
  std::string streamer_id;            // e.g. "STR-LAB01"
  std::string streamer_public_key;    // base64 Ed25519 (must match the streamer's mDNS record)
  std::string streamer_secret_key;    // base64 Ed25519 secret (signs the request; never sent)
  std::string site_id;
  std::string initial_speaker_name;
  std::string setup_code;             // shown to the user by the speaker (QR/screen)
  std::string wifi_ssid;
  std::string wifi_psk;
  std::string speaker_box_public_key; // base64 X25519 from the speaker's mDNS beacon (seal target)
};

struct PairingReply {
  bool ok = false;
  std::string message;
  std::string device_id;  // the speaker's device_id on success
};

// Performs the pairing handshake: seal Wi-Fi creds to the speaker's X25519 key (crypto_box_seal),
// build + sign a PairingRequest (shared PairingRequest::canonicalString(), raw-bytes Ed25519), send
// it as type:"pairing" over the control channel, and parse the pairing_result. The JSON wire keys
// are sealed_wifi / signature (NOT the *_b64 suffixes used by the C++ struct) — matching the
// speaker's pairing handler in Application.cpp.
class PairingClient {
 public:
  PairingClient(control::ILineTransport& transport, std::string host, int port)
      : transport_(transport), host_(std::move(host)), port_(port) {}

  core::Result<PairingReply> pair(const PairingParams& p);

 private:
  control::ILineTransport& transport_;
  std::string host_;
  int port_;
};

}  // namespace nexus::streamer::pairing
