#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Result.h"

namespace nexus::discovery {

// A discovered Streamer on the local network (from the mDNS service `_nexus-streamer._tcp`).
struct StreamerRecord {
  std::string streamer_id;
  std::string host;   // hostname or IP
  int port = 0;
  std::string public_key;  // base64 Ed25519, advertised in the TXT record
};

// The beacon this speaker advertises while in setup mode, so a Streamer can find and pair it.
struct SpeakerBeacon {
  std::string device_id;
  std::string serial_number;
  std::string model;
  std::string software_version;
  std::string box_public_key;  // base64 X25519 key the Streamer seals Wi-Fi creds to
  int control_port = 45455;
  bool setup_mode = true;
};

// OS abstraction for service discovery. On the Pi this is backed by Avahi/mDNS; on dev hosts a
// stub lets the DiscoveryService logic and events be tested off-target. Selected by NEXUS_STUB_HAL.
class IDiscoveryHal {
 public:
  virtual ~IDiscoveryHal() = default;

  // Publish the speaker beacon (setup mode). Idempotent; re-publishing updates the record.
  virtual core::Status publishBeacon(const SpeakerBeacon& beacon) = 0;
  virtual core::Status stopBeacon() = 0;

  // Browse for Streamers advertising the Nexus streamer service. Blocking, bounded snapshot.
  virtual core::Result<std::vector<StreamerRecord>> browseStreamers() = 0;
};

// Stub discovery for development hosts. Records the published beacon and returns a single
// synthetic Streamer so the finder logic can be exercised.
class StubDiscoveryHal : public IDiscoveryHal {
 public:
  core::Status publishBeacon(const SpeakerBeacon& beacon) override {
    published_ = beacon;
    publishing_ = true;
    return core::Status::success();
  }
  core::Status stopBeacon() override {
    publishing_ = false;
    return core::Status::success();
  }
  core::Result<std::vector<StreamerRecord>> browseStreamers() override {
    return std::vector<StreamerRecord>{
        {"STR-LAB01", "streamer.local", 6789, "c3RyZWFtZXItcHVibGljLWtleQ=="}};
  }

  bool publishing() const { return publishing_; }
  const SpeakerBeacon& publishedBeacon() const { return published_; }

 private:
  SpeakerBeacon published_;
  bool publishing_ = false;
};

}  // namespace nexus::discovery
