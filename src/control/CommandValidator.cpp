#include "control/CommandValidator.h"

#include "identity/Crypto.h"

namespace nexus::control {

using core::ErrorCode;
using core::Status;

Status CommandValidator::validate(const Command& c, const std::string& device_id,
                                  const std::string& streamer_public_key_b64,
                                  std::int64_t now_epoch) {
  if (c.command_id.empty()) {
    return Status::error(ErrorCode::InvalidArg, "command missing command_id");
  }
  if (c.command.empty()) {
    return Status::error(ErrorCode::InvalidArg, "command missing command name");
  }
  // target_id is optional on the wire, but if present it must match this device.
  if (!c.target_id.empty() && c.target_id != device_id) {
    return Status::error(ErrorCode::PermissionDenied, "command target mismatch");
  }
  if (c.expires_at != 0 && now_epoch > c.expires_at) {
    return Status::error(ErrorCode::Timeout, "command expired");
  }
  if (streamer_public_key_b64.empty()) {
    return Status::error(ErrorCode::PermissionDenied, "device not paired to a streamer");
  }
  if (c.signature_b64.empty()) {
    return Status::error(ErrorCode::PermissionDenied, "command not signed");
  }

  const std::string canonical = c.canonicalString();
  const std::string canonical_b64 = identity::crypto::toBase64(
      std::vector<std::uint8_t>(canonical.begin(), canonical.end()));
  if (!identity::crypto::verifyEd25519Base64(canonical_b64, c.signature_b64,
                                             streamer_public_key_b64)) {
    return Status::error(ErrorCode::CryptoError, "command signature invalid");
  }
  return Status::success();
}

}  // namespace nexus::control
