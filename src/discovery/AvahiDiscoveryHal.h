#pragma once

#include "discovery/IDiscoveryHal.h"

struct AvahiClient;
struct AvahiSimplePoll;
struct AvahiEntryGroup;

namespace nexus::discovery {

// Real discovery HAL backed by Avahi/mDNS on Raspberry Pi OS. Built when NEXUS_STUB_HAL is off.
// Publishes the speaker's setup beacon as `_nexus-speaker._tcp` (with a TXT record) and browses
// `_nexus-streamer._tcp` to find the paired Streamer. Uses a per-call avahi-simple-poll so the
// synchronous IDiscoveryHal contract holds without owning a background thread.
class AvahiDiscoveryHal : public IDiscoveryHal {
 public:
  static constexpr const char* kSpeakerService = "_nexus-speaker._tcp";
  static constexpr const char* kStreamerService = "_nexus-streamer._tcp";

  AvahiDiscoveryHal();
  ~AvahiDiscoveryHal() override;

  core::Status publishBeacon(const SpeakerBeacon& beacon) override;
  core::Status stopBeacon() override;
  core::Result<std::vector<StreamerRecord>> browseStreamers() override;

 private:
  AvahiSimplePoll* publish_poll_ = nullptr;
  AvahiClient* publish_client_ = nullptr;
  AvahiEntryGroup* group_ = nullptr;
};

}  // namespace nexus::discovery
