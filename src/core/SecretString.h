#pragma once

#include <ostream>
#include <string>
#include <utility>

namespace nexus::core {

// A string wrapper for sensitive values (passwords, tokens, key material handles). It renders
// as "***" whenever serialized or streamed, so a secret can never accidentally reach a log or
// JSON payload. The plaintext is only reachable through the explicit reveal() accessor, which
// makes every such access greppable in review.
class SecretString {
 public:
  SecretString() = default;
  explicit SecretString(std::string value) : value_(std::move(value)) {}

  // Explicit, greppable access to the underlying secret. Use sparingly.
  const std::string& reveal() const { return value_; }
  bool empty() const { return value_.empty(); }
  std::size_t size() const { return value_.size(); }

  static const char* redacted() { return "***"; }

  // Never expose the value implicitly.
  friend std::ostream& operator<<(std::ostream& os, const SecretString&) {
    return os << redacted();
  }

 private:
  std::string value_;
};

}  // namespace nexus::core
