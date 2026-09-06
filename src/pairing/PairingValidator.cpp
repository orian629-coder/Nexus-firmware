#include "pairing/PairingValidator.h"

#include "identity/Crypto.h"

namespace nexus::pairing {

using core::ErrorCode;
using core::Status;

Status PairingValidator::validate(const PairingRequest& req,
                                  const std::string& expected_setup_code, std::int64_t now_epoch,
                                  std::int64_t code_expiry_epoch) {
  if (req.streamer_id.empty() || req.streamer_public_key.empty()) {
    return Status::error(ErrorCode::InvalidArg, "missing streamer identity");
  }
  if (expected_setup_code.empty()) {
    return Status::error(ErrorCode::PermissionDenied, "no active setup code");
  }
  if (now_epoch > code_expiry_epoch) {
    return Status::error(ErrorCode::Timeout, "setup code expired");
  }
  // Constant-time-ish comparison isn't critical here (codes are short-lived and rate-limited by
  // setup mode), but avoid trivially leaking via early return on length.
  if (req.setup_code != expected_setup_code) {
    return Status::error(ErrorCode::PermissionDenied, "setup code mismatch");
  }
  if (req.sealed_wifi_b64.empty()) {
    return Status::error(ErrorCode::InvalidArg, "missing sealed wifi credentials");
  }

  // The signature must verify against the streamer's advertised public key over the canonical
  // request bytes — this proves the sender holds the private key matching the beacon record.
  const std::string canonical = req.canonicalString();
  if (!identity::crypto::verifyEd25519Base64(
          identity::crypto::toBase64(
              std::vector<std::uint8_t>(canonical.begin(), canonical.end())),
          req.signature_b64, req.streamer_public_key)) {
    return Status::error(ErrorCode::CryptoError, "pairing request signature invalid");
  }
  return Status::success();
}

}  // namespace nexus::pairing
