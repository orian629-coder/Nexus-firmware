#pragma once

#include <string>
#include <string_view>

namespace nexus::logging {

// Helpers for scrubbing sensitive data before it reaches a log line. The primary guarantee is
// structural — the Logger API offers no overload that accepts raw key/audio buffers — but these
// helpers cover the cases where a value must appear partially (e.g. a MAC for diagnostics).

// Fully redact a key/secret to a fixed placeholder.
inline std::string redactKey() { return "<redacted-key>"; }

// Mask all but the last octet of a MAC address: "aa:bb:cc:dd:ee:ff" -> "**:**:**:**:**:ff".
std::string maskMac(std::string_view mac);

// Keep the first 2 and last 2 characters of a token, mask the middle: "abcd1234" -> "ab****34".
std::string maskToken(std::string_view token);

}  // namespace nexus::logging
