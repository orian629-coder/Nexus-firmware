#pragma once

#include <string>

#include "core/Result.h"
#include "updater/UpdatePackage.h"

namespace nexus::updater {

// Verifies an update package before it is installed:
//  - the payload's SHA-256 matches the manifest, and
//  - the manifest is signed by the vendor's Ed25519 public key.
// No unsigned or tampered package is ever installed. Pure — fully unit-testable.
class SignatureValidator {
 public:
  explicit SignatureValidator(std::string vendor_public_key_b64)
      : vendor_key_(std::move(vendor_public_key_b64)) {}

  core::Status verify(const UpdatePackage& pkg) const;

 private:
  std::string vendor_key_;
};

}  // namespace nexus::updater
