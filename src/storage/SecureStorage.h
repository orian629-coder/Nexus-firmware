#pragma once

#include <string>

#include "core/Result.h"
#include "core/SecretString.h"

namespace nexus::storage {

// File-backed store for device secrets (private keys, Wi-Fi credentials, pairing material).
// Each secret is a single file under a 0700 directory, written 0600, via the same atomic
// temp+rename+fsync pattern as ConfigManager. This is deliberately simple and dependency-free;
// a later phase may back it with a TPM/secure element behind this same interface.
//
// Secrets never pass through ConfigManager and never reach a log (values are SecretString).
class SecureStorage {
 public:
  explicit SecureStorage(std::string dir = "/var/lib/nexus-speaker/secure");

  const std::string& dir() const { return dir_; }

  core::Status put(const std::string& key, const core::SecretString& value);
  core::Result<core::SecretString> get(const std::string& key) const;
  bool has(const std::string& key) const;
  core::Status erase(const std::string& key);

 private:
  std::string pathFor(const std::string& key) const;
  std::string dir_;
};

}  // namespace nexus::storage
