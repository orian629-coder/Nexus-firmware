#include "discovery/DiscoveryService.h"

#include "logging/Logger.h"

#if NEXUS_STUB_HAL
// StubDiscoveryHal is defined in the interface header.
#else
#include "discovery/AvahiDiscoveryHal.h"
#endif

namespace nexus::discovery {

using core::ErrorCode;
using core::Result;
using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<IDiscoveryHal> makeDefaultHal() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubDiscoveryHal>();
#else
  return std::make_unique<AvahiDiscoveryHal>();
#endif
}
}  // namespace

DiscoveryService::DiscoveryService(core::EventBus* bus, std::unique_ptr<IDiscoveryHal> hal)
    : bus_(bus), hal_(hal ? std::move(hal) : makeDefaultHal()) {}

Status DiscoveryService::start() {
  state_ = ServiceState::Running;
  return Status::success();
}

Status DiscoveryService::stop() {
  if (advertising_) stopAdvertising();
  state_ = ServiceState::Stopped;
  return Status::success();
}

Status DiscoveryService::advertise(const SpeakerBeacon& beacon) {
  Status s = hal_->publishBeacon(beacon);
  if (!s.ok()) {
    NX_LOG_ERROR("discovery", s.code(), "publish beacon failed: " + s.message());
    return s;
  }
  advertising_ = true;
  NX_LOG_INFO("discovery", "advertising beacon device_id=" + beacon.device_id);
  return Status::success();
}

Status DiscoveryService::stopAdvertising() {
  advertising_ = false;
  return hal_->stopBeacon();
}

Result<StreamerRecord> DiscoveryService::findStreamer(const std::string& streamer_id) {
  auto found = hal_->browseStreamers();
  if (!found.ok()) return found.status();
  for (const auto& rec : found.value()) {
    if (rec.streamer_id == streamer_id) {
      NX_LOG_INFO("discovery", "found streamer " + streamer_id + " at " + rec.host);
      if (bus_) {
        bus_->publish(core::Event{core::EventType::StreamerFound, "discovery",
                                  {{"streamer_id", rec.streamer_id},
                                   {"host", rec.host},
                                   {"port", rec.port}}});
      }
      return rec;
    }
  }
  NX_LOG_WARN("discovery", "streamer " + streamer_id + " not found");
  if (bus_) {
    bus_->publish(core::Event{core::EventType::StreamerLost, "discovery",
                              {{"streamer_id", streamer_id}}});
  }
  return Status::error(ErrorCode::NotFound, "streamer not found: " + streamer_id);
}

}  // namespace nexus::discovery
