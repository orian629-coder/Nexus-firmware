#pragma once

#include "core/Result.h"

namespace nexus::amplifier {

// Raw hardware fault flags read from the amplifier (e.g. via GPIO fault/protect pins on the Pi).
struct AmplifierFlags {
  bool fault = false;       // hardware fault line asserted
  bool protection = false;  // amp in self-protection (over-current, DC offset, etc.)
  bool clipping = false;    // output clipping detected
};

// Hardware abstraction for the power amplifier. Two implementations are selected at build time by
// NEXUS_STUB_HAL: a stub (dev hosts / macOS) and an ALSA/GPIO-backed real driver (Pi). This lets
// the amplifier module build and unit-test off-target.
class IAmplifierHal {
 public:
  virtual ~IAmplifierHal() = default;
  virtual core::Status powerOn() = 0;
  virtual core::Status powerOff() = 0;
  virtual core::Status mute(bool on) = 0;
  virtual core::Result<double> readTemperatureCelsius() = 0;
  virtual core::Result<AmplifierFlags> readFlags() = 0;
};

// A no-op HAL used on development hosts. Reports a benign fixed temperature and no faults. Tests
// can subclass to inject temperatures/faults.
class StubAmplifierHal : public IAmplifierHal {
 public:
  core::Status powerOn() override { return core::Status::success(); }
  core::Status powerOff() override { return core::Status::success(); }
  core::Status mute(bool on) override {
    muted = on;
    return core::Status::success();
  }
  core::Result<double> readTemperatureCelsius() override { return temperature; }
  core::Result<AmplifierFlags> readFlags() override { return flags; }

  // Settable by tests to drive the manager's state machine.
  double temperature = 42.0;
  AmplifierFlags flags{};
  bool muted = true;
};

}  // namespace nexus::amplifier
