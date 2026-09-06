#pragma once

#include <string>

namespace nexus::web {

// Guards state-changing API endpoints on the local network. A bearer token is required in the
// Authorization header ("Bearer <token>"). The token is provisioned per device (e.g. derived from
// the device id / a maintenance secret) and shown to the technician. Read-only endpoints are open
// on the trusted LAN; writes require the token. Constant-time comparison avoids trivial timing
// leaks.
class Authentication {
 public:
  explicit Authentication(std::string token) : token_(std::move(token)) {}

  void setToken(std::string token) { token_ = std::move(token); }

  // The device's own token, so the pages it serves can authenticate their state-changing POSTs.
  const std::string& token() const { return token_; }

  // True if the Authorization header carries the correct bearer token.
  bool authorize(const std::string& auth_header) const {
    const std::string prefix = "Bearer ";
    if (auth_header.size() <= prefix.size()) return false;
    if (auth_header.compare(0, prefix.size(), prefix) != 0) return false;
    return constantTimeEquals(auth_header.substr(prefix.size()), token_);
  }

 private:
  static bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
      diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
  }

  std::string token_;
};

}  // namespace nexus::web
