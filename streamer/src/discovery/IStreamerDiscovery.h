#pragma once

#include <string>
#include <vector>

#include "core/Result.h"
#include "discovery/IDiscoveryHal.h"  // reuse SpeakerBeacon / StreamerRecord field definitions

namespace nexus::streamer::discovery {

// What the streamer advertises so a speaker's DiscoveryService can find its paired streamer. Mirror
// of the speaker's StreamerRecord: service `_nexus-streamer._tcp`, TXT streamer_id + public_key.
struct StreamerAdvertisement {
  std::string streamer_id;   // becomes the mDNS service instance name (browseStreamers reads it)
  std::string public_key;    // base64 Ed25519, TXT "public_key" (verifies signed commands)
  int port = 0;              // SRV port (the streamer's control/heartbeat endpoint if any)
};

// A speaker found on the LAN: the beacon's TXT fields plus the resolved host/IP (from mDNS A/SRV),
// which the beacon struct itself doesn't carry. Everything the pairing card needs to auto-fill.
struct DiscoveredSpeaker {
  std::string device_id;
  std::string host;            // resolved IPv4/hostname — the pairing/control target
  std::string box_public_key;  // X25519, to seal Wi-Fi creds
  int control_port = 45455;
  bool setup_mode = false;
};

// The streamer's discovery role is the MIRROR of the speaker's IDiscoveryHal: it PUBLISHES the
// streamer service and BROWSES for speaker setup beacons (`_nexus-speaker._tcp`). Abstracted so the
// pairing/onboarding flow is testable off-target with a stub; the real Avahi implementation is
// compiled only on the Pi (NEXUS_STREAMER_AVAHI).
class IStreamerDiscovery {
 public:
  virtual ~IStreamerDiscovery() = default;

  // Publish (or re-publish) the streamer service. Idempotent.
  virtual core::Status advertise(const StreamerAdvertisement& ad) = 0;
  virtual core::Status stopAdvertising() = 0;

  // Browse for speakers currently advertising a setup beacon. Blocking, bounded snapshot. Returns
  // resolved records (host from mDNS A/SRV + the beacon's TXT fields) ready for the pairing card.
  virtual core::Result<std::vector<DiscoveredSpeaker>> browseSpeakers() = 0;
};

// Stub for dev hosts: records what was advertised and returns a synthetic speaker beacon so the
// onboarding flow can be exercised without Avahi. Mirrors the speaker's StubDiscoveryHal.
class StubStreamerDiscovery : public IStreamerDiscovery {
 public:
  core::Status advertise(const StreamerAdvertisement& ad) override {
    advertised_ = ad;
    advertising_ = true;
    return core::Status::success();
  }
  core::Status stopAdvertising() override {
    advertising_ = false;
    return core::Status::success();
  }
  core::Result<std::vector<DiscoveredSpeaker>> browseSpeakers() override {
    DiscoveredSpeaker s;
    s.device_id = "SPK-STUB01";
    s.host = "192.168.1.50";
    s.box_public_key = "c3R1Yi14MjU1MTkta2V5";  // opaque stub key
    s.control_port = 45455;
    s.setup_mode = true;
    return std::vector<DiscoveredSpeaker>{s};
  }

  bool advertising() const { return advertising_; }
  const StreamerAdvertisement& advertised() const { return advertised_; }

 private:
  StreamerAdvertisement advertised_;
  bool advertising_ = false;
};

}  // namespace nexus::streamer::discovery
