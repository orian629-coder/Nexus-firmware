#include "logging/Redaction.h"

namespace nexus::logging {

std::string maskMac(std::string_view mac) {
  // Find last ':' and keep only the trailing octet.
  auto pos = mac.rfind(':');
  if (pos == std::string_view::npos || pos + 1 >= mac.size()) return "**:**:**:**:**:**";
  return std::string("**:**:**:**:**:") + std::string(mac.substr(pos + 1));
}

std::string maskToken(std::string_view token) {
  if (token.size() <= 4) return "****";
  std::string out;
  out += token.substr(0, 2);
  out.append(token.size() - 4, '*');
  out += token.substr(token.size() - 2);
  return out;
}

}  // namespace nexus::logging
