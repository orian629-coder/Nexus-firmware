#pragma once

// Stub for exporting logs off-device (e.g. bundling for a technician via the local web UI).
// Implemented in Phase 9. Present now so the module tree is complete.

#include <string>

namespace nexus::logging {

class LogExporter {
 public:
  // Produce a redacted, shareable log bundle path. No-op in Phase 1.
  std::string exportBundle() { return {}; }
};

}  // namespace nexus::logging
