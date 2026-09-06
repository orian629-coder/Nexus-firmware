#include "core/ServiceStatus.h"

namespace nexus::core {

const char* toString(ServiceState s) {
  switch (s) {
    case ServiceState::Stopped: return "Stopped";
    case ServiceState::Starting: return "Starting";
    case ServiceState::Running: return "Running";
    case ServiceState::Degraded: return "Degraded";
    case ServiceState::Faulted: return "Faulted";
    case ServiceState::Stopping: return "Stopping";
  }
  return "Unknown";
}

}  // namespace nexus::core
