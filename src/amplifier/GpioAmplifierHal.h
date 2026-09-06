#pragma once

#include <string>

#include "amplifier/IAmplifierHal.h"

struct gpiod_chip;
struct gpiod_line_request;

namespace nexus::amplifier {

// Real amplifier control on the Pi: mute/enable via GPIO output lines, fault/protection sensed via
// GPIO input lines (libgpiod), and temperature read from a sysfs thermal zone. Built only when
// NEXUS_STUB_HAL is off. Pin/zone assignments come from the constructor (wired per board).
class GpioAmplifierHal : public IAmplifierHal {
 public:
  struct Pins {
    std::string chip = "gpiochip0";
    int enable_line = 17;   // amp enable/standby (output)
    int mute_line = 27;     // hardware mute (output)
    int fault_line = 22;    // fault (input, active low)
    int protect_line = 23;  // protection (input, active low)
    std::string thermal_zone = "/sys/class/thermal/thermal_zone0/temp";
  };

  GpioAmplifierHal() : GpioAmplifierHal(Pins{}) {}
  explicit GpioAmplifierHal(Pins pins);
  ~GpioAmplifierHal() override;

  core::Status powerOn() override;
  core::Status powerOff() override;
  core::Status mute(bool on) override;
  core::Result<double> readTemperatureCelsius() override;
  core::Result<AmplifierFlags> readFlags() override;

 private:
  core::Status ensureOpen();

  Pins pins_;
  gpiod_chip* chip_ = nullptr;
  gpiod_line_request* enable_req_ = nullptr;
  gpiod_line_request* mute_req_ = nullptr;
  gpiod_line_request* fault_req_ = nullptr;
  gpiod_line_request* protect_req_ = nullptr;
};

}  // namespace nexus::amplifier
