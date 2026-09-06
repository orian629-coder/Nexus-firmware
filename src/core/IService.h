#pragma once

#include <string>

#include "core/Result.h"
#include "core/ServiceStatus.h"

namespace nexus::core {

// The universal lifecycle contract. Every module's top-level class implements IService so the
// Application can bring services up and down in a fixed order and the Watchdog can monitor them
// uniformly. Implementations must be robust: start()/stop() are idempotent and stop() must not
// throw. A non-critical service failing to start is logged and marked Degraded — it must not
// take down the process.
class IService {
 public:
  virtual ~IService() = default;

  // Short stable identifier, e.g. "config", "network". Used in logs and the service registry.
  virtual std::string name() const = 0;

  // Bring the service up. Idempotent: calling start() on an already-running service is a no-op
  // that returns ok. Returns a non-ok Status on failure (never throws across the boundary).
  virtual Status start() = 0;

  // Bring the service down. Idempotent and must not throw.
  virtual Status stop() = 0;

  // Current lifecycle state.
  virtual ServiceState state() const = 0;

  // Optional liveness probe polled by Watchdog/HealthMonitor. Default: ok.
  virtual Status healthCheck() { return Status::success(); }
};

}  // namespace nexus::core
