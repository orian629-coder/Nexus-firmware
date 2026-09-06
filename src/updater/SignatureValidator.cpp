#include "updater/SignatureValidator.h"

#include "identity/Crypto.h"

namespace nexus::updater {

using core::ErrorCode;
using core::Status;

Status SignatureValidator::verify(const UpdatePackage& pkg) const {
  if (vendor_key_.empty()) {
    return Status::error(ErrorCode::PermissionDenied, "no vendor key configured");
  }
  // 1. Payload integrity: the SHA-256 must match the manifest.
  const std::string actual = identity::crypto::sha256Hex(pkg.payload);
  if (actual != pkg.manifest.payload_sha256) {
    return Status::error(ErrorCode::Corrupt, "payload hash mismatch");
  }
  // 2. Authenticity: the manifest must be signed by the vendor key.
  const std::string canonical = pkg.manifest.canonicalString();
  const std::string canonical_b64 = identity::crypto::toBase64(
      std::vector<std::uint8_t>(canonical.begin(), canonical.end()));
  if (!identity::crypto::verifyEd25519Base64(canonical_b64, pkg.signature_b64, vendor_key_)) {
    return Status::error(ErrorCode::CryptoError, "manifest signature invalid");
  }
  return Status::success();
}

}  // namespace nexus::updater
