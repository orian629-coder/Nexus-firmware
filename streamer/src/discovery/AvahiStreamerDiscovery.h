#pragma once

#include <string>
#include <vector>

#include "discovery/IStreamerDiscovery.h"

// Forward-declare Avahi types to keep this header free of the C library on non-Pi builds.
struct AvahiSimplePoll;
struct AvahiClient;
struct AvahiEntryGroup;

namespace nexus::streamer::discovery {

// Real mDNS via Avahi: publishes `_nexus-streamer._tcp` (TXT public_key) and browses
// `_nexus-speaker._tcp` for setup beacons. The mirror of the speaker's AvahiDiscoveryHal. Uses a
// per-call avahi-simple-poll like the speaker's HAL. Compiled only when NEXUS_STREAMER_AVAHI is on.
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
  AvahiSimplePoll* publish_poll_ = nullptr;
  AvahiClient* publish_client_ = nullptr;
  AvahiEntryGroup* group_ = nullptr;
};

}  // namespace nexus::streamer::discovery
