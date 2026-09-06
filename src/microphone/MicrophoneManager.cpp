#include "microphone/MicrophoneManager.h"

#include "logging/Logger.h"

#if !NEXUS_STUB_HAL
#include "microphone/AlsaMicrophoneHal.h"
#endif

namespace nexus::microphone {

using core::ErrorCode;
using core::Result;
using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IMicrophoneHal> makeDefaultMicHal() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubMicrophoneHal>();
#else
  return std::make_unique<AlsaMicrophoneHal>();
#endif
}
}  // namespace

MicrophoneManager::MicrophoneManager(core::EventBus* bus, std::unique_ptr<IMicrophoneHal> hal)
    : bus_(bus), hal_(hal ? std::move(hal) : makeDefaultMicHal()) {}

Status MicrophoneManager::start() {
  Status s = hal_->open();
  open_ = s.ok();
  state_ = s.ok() ? ServiceState::Running : ServiceState::Degraded;
  if (!s.ok()) NX_LOG_ERROR("microphone", s.code(), "mic open failed: " + s.message());
  return s;
}

Status MicrophoneManager::stop() {
  if (open_) hal_->close();
  open_ = false;
  state_ = ServiceState::Stopped;
  return Status::success();
}

Result<std::vector<std::int16_t>> MicrophoneManager::capture(std::size_t frames) {
  return hal_->capture(frames);
}

Result<double> MicrophoneManager::measureSpl(std::size_t frames) {
  auto pcm = hal_->capture(frames);
  if (!pcm.ok()) return pcm.status();
  return spl_.splDb(pcm.value());
}

Result<double> MicrophoneManager::updateAmbient(std::size_t frames) {
  auto pcm = hal_->capture(frames);
  if (!pcm.ok()) return pcm.status();
  return noise_.update(pcm.value());
}

Status MicrophoneManager::selfTest() {
  auto pcm = hal_->capture(2400);
  if (!pcm.ok()) {
    if (bus_) bus_->publish(core::Event{core::EventType::MicFailure, "microphone"});
    return pcm.status();
  }
  // A working mic produces a buffer of the expected size. (On dev hosts the stub returns silence,
  // which still validates the capture path; a real acoustic check runs on hardware in Phase 7.)
  if (pcm.value().empty()) {
    if (bus_) bus_->publish(core::Event{core::EventType::MicFailure, "microphone"});
    return Status::error(ErrorCode::IoError, "microphone returned no samples");
  }
  NX_LOG_INFO("microphone", "mic self-test ok (" + std::to_string(pcm.value().size()) +
                                " samples)");
  return Status::success();
}

}  // namespace nexus::microphone
