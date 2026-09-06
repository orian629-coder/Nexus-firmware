#pragma once

#include <memory>
#include <string>

#include "core/EventBus.h"
#include "core/IService.h"
#include "discovery/IDiscoveryHal.h"

namespace nexus::discovery {

// Finds the paired Streamer on the local network and advertises this speaker's beacon while
// unpaired (setup mode). Emits StreamerFound when a matching streamer is located, StreamerLost
// otherwise. mDNS specifics live behind IDiscoveryHal, so the finder logic is testable off-target.
class DiscoveryService : public core::IService {
 public:
  explicit DiscoveryService(core::EventBus* bus, std::unique_ptr<IDiscoveryHal> hal = nullptr);

  std::string name() const override { return "discovery"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Enter setup mode: advertise this speaker's beacon so a Streamer can discover and pair it.
  core::Status advertise(const SpeakerBeacon& beacon);
  core::Status stopAdvertising();

  // Once paired, search for the given streamer_id. On a match, publishes StreamerFound (with host,
  // port, public_key) and returns the record; otherwise publishes StreamerLost.
  core::Result<StreamerRecord> findStreamer(const std::string& streamer_id);

 private:
  core::EventBus* bus_;
  std::unique_ptr<IDiscoveryHal> hal_;
  core::ServiceState state_ = core::ServiceState::Stopped;
  bool advertising_ = false;
};

}  // namespace nexus::discovery
