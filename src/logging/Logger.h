#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "core/ErrorCodes.h"
#include "core/Result.h"
#include "core/SecretString.h"

namespace spdlog {
class logger;
}

namespace nexus::logging {

enum class Level { Debug, Info, Warning, Error, Critical };

Level levelFromString(std::string_view s);
const char* toString(Level l);

struct LogConfig {
  std::string file_path = "/var/log/nexus-speaker/speaker.log";
  Level level = Level::Info;
  std::size_t max_file_bytes = 10 * 1024 * 1024;  // rotate at 10 MB
  std::size_t max_files = 5;                       // keep 5 rotated files
  bool to_stderr = true;   // systemd journal captures stderr
  bool to_file = true;     // file sink for offline diagnostics
};

// Process-wide logging facade over spdlog. Enforces the secrecy rules structurally: there is no
// API that accepts raw key material or audio buffers, and secrets can only be passed as
// core::SecretString (which renders redacted). Every error log carries an ErrorCode; command
// flows carry a command_id. spdlog types never appear in this header, so the backend is
// swappable.
class Logger {
 public:
  static Logger& instance();

  // Idempotent; safe to call once at startup. Returns non-ok if the file sink can't be created.
  core::Status init(const LogConfig& config);
  void setLevel(Level level);
  bool initialized() const { return impl_ != nullptr; }

  void log(Level level, std::string_view module, std::string_view message,
           core::ErrorCode code = core::ErrorCode::Ok);

  // Command-scoped: always records command_id, never the payload or signature.
  void logCommand(Level level, std::string_view module, std::string_view command_id,
                  std::string_view message, core::ErrorCode code = core::ErrorCode::Ok);

  // Convenience for a redacted secret reference in a message (renders "***").
  static const char* redact(const core::SecretString&) { return core::SecretString::redacted(); }

  void flush();
  void shutdown();

 private:
  Logger() = default;
  std::shared_ptr<spdlog::logger> impl_;
  Level level_ = Level::Info;
};

}  // namespace nexus::logging

// Convenience macros. `mod` is the module tag; error/critical variants take an ErrorCode.
#define NX_LOG_DEBUG(mod, msg) \
  ::nexus::logging::Logger::instance().log(::nexus::logging::Level::Debug, (mod), (msg))
#define NX_LOG_INFO(mod, msg) \
  ::nexus::logging::Logger::instance().log(::nexus::logging::Level::Info, (mod), (msg))
#define NX_LOG_WARN(mod, msg) \
  ::nexus::logging::Logger::instance().log(::nexus::logging::Level::Warning, (mod), (msg))
#define NX_LOG_ERROR(mod, code, msg) \
  ::nexus::logging::Logger::instance().log(::nexus::logging::Level::Error, (mod), (msg), (code))
#define NX_LOG_CRIT(mod, code, msg) \
  ::nexus::logging::Logger::instance().log(::nexus::logging::Level::Critical, (mod), (msg), (code))
