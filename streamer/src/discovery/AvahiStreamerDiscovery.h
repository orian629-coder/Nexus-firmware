#pragma once

#include <string>
#include <vector>

#include "discovery/IStreamerDiscovery.h"

// Forward-declare Avahi types to keep this header free of the C library on non-Pi builds.
struct AvahiThreadedPoll;
struct AvahiClient;

namespace nexus::streamer::discovery {

// Publish state owned across the advertisement's lifetime; defined in the .cpp and shared with the
// Avahi callbacks (passed as their userdata) so registration survives daemon restarts.
struct PublishCtx;

// Real mDNS via Avahi: publishes `_nexus-streamer._tcp` (TXT streamer_id + public_key) and browses
// `_nexus-speaker._tcp` for setup beacons. The mirror of the speaker's AvahiDiscoveryHal.
//
// advertise() runs the Avahi client on a dedicated background thread (avahi-threaded-poll) so the
// entry group is driven to ESTABLISHED and stays announced for the streamer's whole lifetime, and is
// re-published automatically if avahi-daemon restarts. (The previous implementation pumped the poll
// once and returned, so the async registration never completed and nothing stayed on the wire.)
// browseSpeakers() remains a one-shot bounded snapshot on its own simple-poll. Compiled only when
// NEXUS_STREAMER_AVAHI is on.
class AvahiStreamerDiscovery : public IStreamerDiscovery {
 public:
  static constexpr const char* kSpeakerService = "_nexus-speaker._tcp";
  static constexpr const char* kStreamerService = "_nexus-streamer._tcp";

  AvahiStreamerDiscovery() = default;
  ~AvahiStreamerDiscovery() override;

  core::Status advertise(const StreamerAdvertisement& ad) override;
  core::Status stopAdvertising() override;
  core::Result<std::vector<DiscoveredSpeaker>> browseSpeakers() override;

 private:
  AvahiThreadedPoll* publish_poll_ = nullptr;
  AvahiClient* publish_client_ = nullptr;
  PublishCtx* publish_ = nullptr;  // heap-owned; freed in stopAdvertising (after the poll stops)
};

}  // namespace nexus::streamer::discovery
