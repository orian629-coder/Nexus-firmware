#pragma once

#include <string>
#include <utility>

#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::core {

// Base for modules not yet implemented. Provides a compiling, safe no-op that satisfies the
// IService contract: start() marks the service Running, stop() marks it Stopped. This lets the
// full module tree build from day one, lets the Application start/stop everything in order, and
// lets the watchdog see stubs as healthy. Filling in a real module means replacing the body —
// the wiring in Application and CMake never changes.
class StubService : public IService {
 public:
  StubService(std::string name, EventBus* bus) : name_(std::move(name)), bus_(bus) {}

  std::string name() const override { return name_; }

  Status start() override {
    state_ = ServiceState::Running;
    return Status::success();
  }

  Status stop() override {
    state_ = ServiceState::Stopped;
    return Status::success();
  }

  ServiceState state() const override { return state_; }

 protected:
  EventBus* bus() const { return bus_; }

 private:
  std::string name_;
  EventBus* bus_;
  ServiceState state_ = ServiceState::Stopped;
};

// Declares a concrete stub module class deriving from StubService with a fixed name.
#define NEXUS_STUB_SERVICE(ClassName, ServiceName)                       \
  class ClassName : public ::nexus::core::StubService {                  \
   public:                                                              \
    explicit ClassName(::nexus::core::EventBus* bus)                     \
        : ::nexus::core::StubService(ServiceName, bus) {}               \
  }

}  // namespace nexus::core
