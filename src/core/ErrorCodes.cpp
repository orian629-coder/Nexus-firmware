#include "core/ErrorCodes.h"

namespace nexus::core {

std::string_view describe(ErrorCode c) {
  switch (c) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::Unknown: return "unknown error";
    case ErrorCode::InvalidArg: return "invalid argument";
    case ErrorCode::NotFound: return "not found";
    case ErrorCode::IoError: return "I/O error";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::PermissionDenied: return "permission denied";
    case ErrorCode::Corrupt: return "corrupt data";
    case ErrorCode::NotImplemented: return "not implemented";
    case ErrorCode::AlreadyExists: return "already exists";
    case ErrorCode::LogInitFailed: return "logger initialization failed";
    case ErrorCode::ConfigNotFound: return "config file not found";
    case ErrorCode::ConfigParseError: return "config parse error";
    case ErrorCode::ConfigValidationFailed: return "config validation failed";
    case ErrorCode::ConfigWriteFailed: return "config write failed";
    case ErrorCode::ConfigCorrupt: return "config corrupt";
    case ErrorCode::IllegalStateTransition: return "illegal state transition";
    case ErrorCode::ServiceStartFailed: return "service start failed";
    case ErrorCode::CryptoError: return "crypto error";
    case ErrorCode::IdentityMissing: return "device identity missing";
    case ErrorCode::IdentityCorrupt: return "device identity corrupt";
    case ErrorCode::KeyGenerationFailed: return "key generation failed";
    case ErrorCode::SigningFailed: return "signing failed";
    case ErrorCode::SecureStoreError: return "secure store error";
  }
  return "unrecognized error code";
}

}  // namespace nexus::core
