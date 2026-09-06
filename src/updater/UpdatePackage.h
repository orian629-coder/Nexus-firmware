#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nexus::updater {

// A downloaded update package: the binary payload plus a manifest describing it. The manifest is
// signed by the vendor's Ed25519 key; the signature covers the manifest's canonical bytes, which
// include the payload's SHA-256 so a tampered payload is rejected.
struct UpdateManifest {
  std::string version;              // e.g. "1.1.0"
  std::string min_hardware_version; // compatibility gate
  std::string payload_sha256;       // hex digest of the payload
  std::int64_t size_bytes = 0;

  // Canonical bytes the signature is computed over (fixed field order).
  std::string canonicalString() const {
    return version + "\n" + min_hardware_version + "\n" + payload_sha256 + "\n" +
           std::to_string(size_bytes);
  }
};

struct UpdatePackage {
  UpdateManifest manifest;
  std::vector<std::uint8_t> payload;  // the new binary
  std::string signature_b64;          // Ed25519 over manifest.canonicalString()
};

}  // namespace nexus::updater
