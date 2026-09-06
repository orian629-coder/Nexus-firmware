#pragma once

#include <string_view>

namespace nexus::core {

// Canonical error-code registry. Ranges are reserved per module so a code alone identifies
// its origin. Every Error/Critical log SHOULD carry a code. Keep values stable — they are a
// diagnostic contract, referenced by docs/error-codes.md and by operators reading logs.
enum class ErrorCode : int {
  Ok = 0,

  // 1000–1099 core
  Unknown = 1000,
  InvalidArg = 1001,
  NotFound = 1002,
  IoError = 1003,
  Timeout = 1004,
  PermissionDenied = 1005,
  Corrupt = 1006,
  NotImplemented = 1007,
  AlreadyExists = 1008,

  // 1100–1199 logging
  LogInitFailed = 1100,

  // 1200–1299 config
  ConfigNotFound = 1200,
  ConfigParseError = 1201,
  ConfigValidationFailed = 1202,
  ConfigWriteFailed = 1203,
  ConfigCorrupt = 1204,

  // 1300–1399 system / state machine
  IllegalStateTransition = 1300,
  ServiceStartFailed = 1301,

  // 1400–1499 identity / crypto
  CryptoError = 1400,
  IdentityMissing = 1401,
  IdentityCorrupt = 1402,
  KeyGenerationFailed = 1403,
  SigningFailed = 1404,
  SecureStoreError = 1405,
};

constexpr int toInt(ErrorCode c) { return static_cast<int>(c); }

std::string_view describe(ErrorCode c);

}  // namespace nexus::core
