#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "control/Command.h"
#include "core/Result.h"

namespace nexus::streamer::control {

// Builds signed commands the speaker's CommandValidator will accept. It reuses the speaker's own
// control::Command struct (and its canonicalString()) so the byte layout is shared by construction,
// and signs with identity::crypto::signEd25519Base64 — the exact counterpart of the speaker's
// verifyEd25519Base64. The signature is over the RAW canonical bytes (the speaker's toBase64 in
// CommandValidator cancels against verifyEd25519Base64's decode), so we must NOT double-encode.
//
// The signer holds the streamer's base64 Ed25519 secret key (from SecureStorage at runtime; passed
// in directly in tests). target_id is the destination speaker's device_id and is part of the signed
// bytes, so each speaker in a group gets its own signed command.
class CommandSigner {
 public:
  explicit CommandSigner(std::string secret_key_b64)
      : secret_key_b64_(std::move(secret_key_b64)) {}

  // Build a fully-signed Command. command_id must be unique per logical command (retries reuse the
  // same id for idempotency). expires_at/timestamp are epoch seconds; the caller supplies them so
  // the clock source stays injectable and testable.
  core::Result<nexus::control::Command> build(const std::string& command_id,
                                              const std::string& target_id,
                                              const std::string& command,
                                              const nlohmann::json& payload,
                                              std::int64_t expires_at,
                                              std::int64_t timestamp) const;

 private:
  std::string secret_key_b64_;
};

}  // namespace nexus::streamer::control
