#pragma once

// Log rotation is handled by spdlog's rotating_file_sink (size + count) configured in
// Logger::init. This header is a placeholder for a future time-based rotation trigger and a
// SIGHUP reopen hook for environments that use external logrotate instead of journald.
// (Phase 9.)

namespace nexus::logging {

class LogRotation {
 public:
  // Reopen the log file on demand (e.g. after external logrotate moved it). No-op in Phase 1.
  void reopen() {}
};

}  // namespace nexus::logging
