#include "amplifier/AmplifierManager.h"

#include "logging/Logger.h"

#if !NEXUS_STUB_HAL
#include "amplifier/GpioAmplifierHal.h"
#endif

namespace nexus::amplifier {

using core::ErrorCode;
using core::Event;
using core::EventType;
using core::Result;
using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IAmplifierHal> makeDefaultAmpHal() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubAmplifierHal>();
#else
  return std::make_unique<GpioAmplifierHal>();
#endif
}
}  // namespace

const char* toString(AmpState s) {
  switch (s) {
    case AmpState::Off: return "OFF";
    case AmpState::Starting: return "STARTING";
    case AmpState::Ready: return "READY";
    case AmpState::Muted: return "MUTED";
    case AmpState::Protection: return "PROTECTION";
    case AmpState::Overheated: return "OVERHEATED";
    case AmpState::Fault: return "FAULT";
  }
  return "UNKNOWN";
}

AmplifierManager::AmplifierManager(core::EventBus* bus, std::unique_ptr<IAmplifierHal> hal,
                                   double warn_celsius, double shutdown_celsius)
    : bus_(bus),
      hal_(hal ? std::move(hal) : makeDefaultAmpHal()),
      warn_celsius_(warn_celsius),
      shutdown_celsius_(shutdown_celsius) {}

void AmplifierManager::transition(AmpState to, const char* reason) {
  // Caller holds mutex_.
  if (amp_state_ == to) return;
  NX_LOG_INFO("amplifier", std::string("amp ") + toString(amp_state_) + " -> " + toString(to) +
                               " (" + reason + ")");
  amp_state_ = to;
}

Status AmplifierManager::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Power on muted (spec: mute during boot, unmute after stability).
  Status p = hal_->powerOn();
  if (!p.ok()) {
    // No controllable amplifier (e.g. a DAC-only HAT with no enable/mute GPIO, or the lines are
    // owned elsewhere). This is not a fault — audio still flows through the DAC. Run in a
    // "no-hardware" mode where mute/unmute are no-ops so playback isn't blocked.
    no_hardware_ = true;
    transition(AmpState::Ready, "no controllable amplifier — DAC-direct mode");
    svc_state_ = ServiceState::Running;
    NX_LOG_WARN("amplifier", "no amplifier GPIO (" + p.message() + "); running DAC-direct");
    return Status::success();
  }
  hal_->mute(true);
  transition(AmpState::Starting, "power on (muted)");
  svc_state_ = ServiceState::Running;
  return Status::success();
}

Status AmplifierManager::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!no_hardware_) {
    hal_->mute(true);
    hal_->powerOff();
  }
  transition(AmpState::Off, "service stop");
  svc_state_ = ServiceState::Stopped;
  return Status::success();
}

AmpState AmplifierManager::ampState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return amp_state_;
}

Status AmplifierManager::unmute() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (no_hardware_) return Status::success();  // DAC-direct: nothing to unmute
  if (amp_state_ == AmpState::Fault || amp_state_ == AmpState::Overheated ||
      amp_state_ == AmpState::Protection) {
    return Status::error(ErrorCode::PermissionDenied, "cannot unmute: amp not healthy");
  }
  Status s = hal_->mute(false);
  if (!s.ok()) return s;
  transition(AmpState::Ready, "unmute");
  return Status::success();
}

Status AmplifierManager::mute() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (no_hardware_) return Status::success();
  Status s = hal_->mute(true);
  if (!s.ok()) return s;
  if (amp_state_ == AmpState::Ready || amp_state_ == AmpState::Starting) {
    transition(AmpState::Muted, "mute");
  }
  return Status::success();
}

Result<double> AmplifierManager::poll() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (no_hardware_) return 0.0;  // no thermal/fault sensing without amp hardware

  auto flags = hal_->readFlags();
  if (flags.ok()) {
    const auto& f = flags.value();
    if (f.fault && amp_state_ != AmpState::Fault) {
      hal_->mute(true);
      transition(AmpState::Fault, "hardware fault");
      if (bus_) bus_->publish(Event{EventType::AmplifierFault, "amplifier"});
    } else if (f.protection && amp_state_ != AmpState::Protection &&
               amp_state_ != AmpState::Fault) {
      hal_->mute(true);
      transition(AmpState::Protection, "amp protection");
      if (bus_) bus_->publish(Event{EventType::AmplifierProtection, "amplifier"});
    }
    if (f.clipping && bus_) bus_->publish(Event{EventType::OutputClipping, "amplifier"});
  }

  auto temp = hal_->readTemperatureCelsius();
  if (!temp.ok()) return temp;
  const double t = temp.value();

  if (t >= shutdown_celsius_) {
    if (amp_state_ != AmpState::Overheated) {
      hal_->mute(true);
      transition(AmpState::Overheated, "thermal shutdown");
      if (bus_) bus_->publish(Event{EventType::AmplifierOverheat, "amplifier",
                                    {{"temperature", t}}});
    }
  } else if (t >= warn_celsius_) {
    if (!warned_) {
      warned_ = true;
      if (bus_) bus_->publish(Event{EventType::TempWarning, "amplifier", {{"temperature", t}}});
    }
  } else {
    warned_ = false;
    // Recovered from an over-temp condition once back below the warning threshold.
    if (amp_state_ == AmpState::Overheated) transition(AmpState::Muted, "cooled down");
  }
  return t;
}

Status AmplifierManager::healthCheck() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (amp_state_ == AmpState::Fault) {
    return Status::error(ErrorCode::Unknown, "amplifier fault");
  }
  return Status::success();
}

}  // namespace nexus::amplifier
