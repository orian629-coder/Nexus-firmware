#pragma once

#include <string>

#include "core/Result.h"
#include "updater/UpdatePackage.h"

namespace nexus::updater {

// Source of update packages. The real implementation downloads from a URL (the Streamer relays the
// update location); the stub returns an in-memory package so the OTA flow is fully testable without
// a network. Selected by NEXUS_STUB_HAL.
class IUpdateSource {
 public:
  virtual ~IUpdateSource() = default;
  // Fetch the package described by `location` (URL or path).
  virtual core::Result<UpdatePackage> fetch(const std::string& location) = 0;
};

// In-process source for dev/tests: holds a package to return, or a failure to simulate.
class StubUpdateSource : public IUpdateSource {
 public:
  core::Result<UpdatePackage> fetch(const std::string&) override {
    if (fail) return core::Status::error(core::ErrorCode::IoError, "download failed");
    return package;
  }
  UpdatePackage package;
  bool fail = false;
};

}  // namespace nexus::updater
