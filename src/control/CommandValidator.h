#pragma once

#include <cstdint>
#include <string>

#include "control/Command.h"
#include "core/Result.h"

namespace nexus::control {

// Validates an inbound command before it is executed:
//  - required fields present (command_id, command),
//  - target_id matches this device (when provided),
//  - not expired (now <= expires_at),
//  - Ed25519 signature verifies against the paired streamer's public key.
// Pure logic — no I/O — so it is fully unit-testable.
class CommandValidator {
 public:
  static core::Status validate(const Command& c, const std::string& device_id,
                               const std::string& streamer_public_key_b64, std::int64_t now_epoch);
};

}  // namespace nexus::control
