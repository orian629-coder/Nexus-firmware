#pragma once

#include <chrono>
#include <string>

#include "core/ErrorCodes.h"

namespace nexus::core {

enum class ServiceState {
  Stopped,
  Starting,
  Running,
  Degraded,   // running but impaired; not a hard failure
  Faulted,    // failed to start or crashed
  Stopping,
};

const char* toString(ServiceState s);

// Cheap, non-blocking snapshot of a service's health, polled by the watchdog / health monitor.
struct ServiceStatus {
  ServiceState state = ServiceState::Stopped;
  std::string detail;                     // human-readable, never contains secrets
  ErrorCode error_code = ErrorCode::Ok;   // meaningful when Faulted/Degraded
  std::chrono::steady_clock::time_point last_ok{};
};

}  // namespace nexus::core
