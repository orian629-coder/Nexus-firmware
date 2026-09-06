#include "amplifier/GpioAmplifierHal.h"

#include <gpiod.h>

#include <fstream>

#include "logging/Logger.h"

// Implemented against libgpiod v2 (Bookworm/Trixie). v2 replaces the per-line request API with
// line-request objects built from line-settings + line-config.

namespace nexus::amplifier {

using core::ErrorCode;
using core::Result;
using core::Status;

namespace {
constexpr const char* kConsumer = "nexus-speaker";

// Build a request for a single line with the given direction/initial value.
gpiod_line_request* requestLine(gpiod_chip* chip, unsigned int offset,
                                gpiod_line_direction dir, gpiod_line_value initial) {
  gpiod_line_settings* settings = gpiod_line_settings_new();
  if (!settings) return nullptr;
  gpiod_line_settings_set_direction(settings, dir);
  if (dir == GPIOD_LINE_DIRECTION_OUTPUT) {
    gpiod_line_settings_set_output_value(settings, initial);
  }

  gpiod_line_config* line_cfg = gpiod_line_config_new();
  gpiod_request_config* req_cfg = gpiod_request_config_new();
  gpiod_line_request* req = nullptr;
  if (line_cfg && req_cfg &&
      gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) == 0) {
    gpiod_request_config_set_consumer(req_cfg, kConsumer);
    req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
  }
  if (req_cfg) gpiod_request_config_free(req_cfg);
  if (line_cfg) gpiod_line_config_free(line_cfg);
  if (settings) gpiod_line_settings_free(settings);
  return req;
}
}  // namespace

GpioAmplifierHal::GpioAmplifierHal(Pins pins) : pins_(std::move(pins)) {}

GpioAmplifierHal::~GpioAmplifierHal() {
  if (enable_req_) gpiod_line_request_release(enable_req_);
  if (mute_req_) gpiod_line_request_release(mute_req_);
  if (fault_req_) gpiod_line_request_release(fault_req_);
  if (protect_req_) gpiod_line_request_release(protect_req_);
  if (chip_) gpiod_chip_close(chip_);
}

Status GpioAmplifierHal::ensureOpen() {
  // Consider "open" only when every line request succeeded — otherwise a partially-opened chip
  // would leave null request handles that crash gpiod's set/get value calls.
  if (chip_ && enable_req_ && mute_req_ && fault_req_ && protect_req_) return Status::success();
  if (!chip_) {
    const std::string path = "/dev/" + pins_.chip;
    chip_ = gpiod_chip_open(path.c_str());
    if (!chip_) return Status::error(ErrorCode::IoError, "gpiod_chip_open " + path);
  }

  if (!enable_req_)
    enable_req_ = requestLine(chip_, static_cast<unsigned int>(pins_.enable_line),
                              GPIOD_LINE_DIRECTION_OUTPUT, GPIOD_LINE_VALUE_INACTIVE);
  if (!mute_req_)
    mute_req_ = requestLine(chip_, static_cast<unsigned int>(pins_.mute_line),
                            GPIOD_LINE_DIRECTION_OUTPUT, GPIOD_LINE_VALUE_ACTIVE);
  if (!fault_req_)
    fault_req_ = requestLine(chip_, static_cast<unsigned int>(pins_.fault_line),
                             GPIOD_LINE_DIRECTION_INPUT, GPIOD_LINE_VALUE_INACTIVE);
  if (!protect_req_)
    protect_req_ = requestLine(chip_, static_cast<unsigned int>(pins_.protect_line),
                               GPIOD_LINE_DIRECTION_INPUT, GPIOD_LINE_VALUE_INACTIVE);
  if (!enable_req_ || !mute_req_ || !fault_req_ || !protect_req_) {
    return Status::error(ErrorCode::IoError, "gpiod line request failed (lines busy?)");
  }
  return Status::success();
}

Status GpioAmplifierHal::powerOn() {
  if (Status s = ensureOpen(); !s.ok()) return s;
  if (gpiod_line_request_set_value(enable_req_, static_cast<unsigned int>(pins_.enable_line),
                                   GPIOD_LINE_VALUE_ACTIVE) < 0) {
    return Status::error(ErrorCode::IoError, "enable set failed");
  }
  NX_LOG_INFO("amplifier", "amp enabled (gpio)");
  return Status::success();
}

Status GpioAmplifierHal::powerOff() {
  if (Status s = ensureOpen(); !s.ok()) return s;
  gpiod_line_request_set_value(mute_req_, static_cast<unsigned int>(pins_.mute_line),
                               GPIOD_LINE_VALUE_ACTIVE);
  gpiod_line_request_set_value(enable_req_, static_cast<unsigned int>(pins_.enable_line),
                               GPIOD_LINE_VALUE_INACTIVE);
  return Status::success();
}

Status GpioAmplifierHal::mute(bool on) {
  if (Status s = ensureOpen(); !s.ok()) return s;
  if (gpiod_line_request_set_value(mute_req_, static_cast<unsigned int>(pins_.mute_line),
                                   on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) < 0) {
    return Status::error(ErrorCode::IoError, "mute set failed");
  }
  return Status::success();
}

Result<double> GpioAmplifierHal::readTemperatureCelsius() {
  std::ifstream in(pins_.thermal_zone);
  if (!in) return Status::error(ErrorCode::IoError, "cannot read thermal zone");
  long millidegrees = 0;
  in >> millidegrees;
  if (!in) return Status::error(ErrorCode::IoError, "thermal parse failed");
  return static_cast<double>(millidegrees) / 1000.0;
}

Result<AmplifierFlags> GpioAmplifierHal::readFlags() {
  if (Status s = ensureOpen(); !s.ok()) return s;
  AmplifierFlags flags;
  // Fault/protect are active-low on typical amp boards: INACTIVE (0) = asserted.
  gpiod_line_value f = gpiod_line_request_get_value(fault_req_,
                                                    static_cast<unsigned int>(pins_.fault_line));
  gpiod_line_value p = gpiod_line_request_get_value(protect_req_,
                                                    static_cast<unsigned int>(pins_.protect_line));
  if (f == GPIOD_LINE_VALUE_ERROR || p == GPIOD_LINE_VALUE_ERROR) {
    return Status::error(ErrorCode::IoError, "gpio read failed");
  }
  flags.fault = (f == GPIOD_LINE_VALUE_INACTIVE);
  flags.protection = (p == GPIOD_LINE_VALUE_INACTIVE);
  return flags;
}

}  // namespace nexus::amplifier
