#pragma once

#include <cstdint>
#include <string>

#include "core/Result.h"
#include "pairing/PairingTypes.h"

namespace nexus::pairing {

// Validates an inbound pairing request: the setup code must match the active one and be unexpired,
// and the request must carry a valid Ed25519 signature made with the streamer's advertised key
// (proving possession). Pure logic — no I/O — so it is fully unit-testable.
class PairingValidator {
 public:
  // `expected_setup_code` is the code currently displayed/active on the speaker; `now_epoch` and
  // `code_expiry_epoch` bound its validity window.
  static core::Status validate(const PairingRequest& req, const std::string& expected_setup_code,
                               std::int64_t now_epoch, std::int64_t code_expiry_epoch);
};

}  // namespace nexus::pairing
