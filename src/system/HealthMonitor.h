#pragma once

#include "core/StubService.h"

namespace nexus::system {

// Collects system-health signals (CPU, RAM, temperature, disk) and raises alerts. Stub in
// Phase 1; real probes arrive with diagnostics/status in later phases.
NEXUS_STUB_SERVICE(HealthMonitor, "health_monitor");

}  // namespace nexus::system
