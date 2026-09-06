#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "core/IService.h"
#include "core/Result.h"
#include "discovery/IStreamerDiscovery.h"

namespace nexus::streamer::discovery {

// Publishes this streamer as `_nexus-streamer._tcp` so speakers can find it.
//
// The advertise() implementation already existed and was simply never called, which meant discovery
// only worked in one direction: the streamer could find speakers, but a speaker browsing for its
// streamer found nothing. Every speaker had to be told the streamer's address by hand.
//
// The wire contract is asymmetric and easy to get wrong, so it is asserted in tests rather than
// eyeballed: the speaker reads the streamer id from the mDNS INSTANCE NAME and the key from a TXT
// record named exactly "public_key". A mismatch here fails silently — the speaker simply never sees
// a streamer — so it must be verified, not assumed.
class DiscoveryService : public core::IService {
 public:
  DiscoveryService(std::shared_ptr<IStreamerDiscovery> discovery, std::string streamer_id,
                   std::string public_key_b64, int port)
      : discovery_(std::move(discovery)),
        streamer_id_(std::move(streamer_id)),
        public_key_b64_(std::move(public_key_b64)),
        port_(port) {}

  ~DiscoveryService() override { stop(); }

  std::string name() const override { return "discovery"; }

  core::Status start() override {
    if (!discovery_) {
      // No mDNS on this build (a dev Mac without Avahi). Not a failure: the streamer still works,
      // speakers just have to be added by address.
      state_ = core::ServiceState::Degraded;
      return core::Status::success();
    }
    if (advertising_.exchange(true)) return core::Status::success();

    StreamerAdvertisement ad;
    ad.streamer_id = streamer_id_;
    ad.public_key = public_key_b64_;
    ad.port = port_;
    const core::Status s = discovery_->advertise(ad);
    state_ = s.ok() ? core::ServiceState::Running : core::ServiceState::Degraded;
    return s;
  }

  core::Status stop() override {
    if (!advertising_.exchange(false)) {
      state_ = core::ServiceState::Stopped;
      return core::Status::success();
    }
    if (discovery_) discovery_->stopAdvertising();
    state_ = core::ServiceState::Stopped;
    return core::Status::success();
  }

  core::ServiceState state() const override { return state_; }

 private:
  std::shared_ptr<IStreamerDiscovery> discovery_;
  std::string streamer_id_;
  std::string public_key_b64_;
  int port_;
  std::atomic<bool> advertising_{false};
  std::atomic<core::ServiceState> state_{core::ServiceState::Stopped};
};

}  // namespace nexus::streamer::discovery
