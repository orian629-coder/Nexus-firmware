#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
  ~DiscoveryService() override;

  std::string name() const override { return "discovery"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Enter setup mode: advertise this speaker's beacon so a Streamer can discover and pair it.
  core::Status advertise(const SpeakerBeacon& beacon);
  core::Status stopAdvertising();

  // One-shot search for the given streamer_id. On a match, publishes StreamerFound (with host,
  // port, public_key) and returns the record; otherwise publishes StreamerLost. When
  // expected_public_key is non-empty, a record whose TXT public_key differs is rejected as a
  // possible spoof (an id match alone is not enough to trust it).
  core::Result<StreamerRecord> findStreamer(const std::string& streamer_id,
                                            const std::string& expected_public_key = "");

  // Continuous search: browse for the target streamer on a background worker, retrying every
  // `interval` until a verified match is found, at which point it publishes StreamerFound once and
  // stops itself. Misses are silent (no StreamerLost storm while still looking). Idempotent — a
  // second call restarts the worker on the new target. stopSearching() cancels it.
  void startSearching(const std::string& streamer_id, const std::string& expected_public_key,
                      std::chrono::milliseconds interval = std::chrono::seconds(3));
  void stopSearching();
  bool searching() const { return searching_.load(); }

 private:
  // Browse once and return the record matching streamer_id (and expected_public_key, if given).
  // Publishes nothing — callers decide which events to emit.
  core::Result<StreamerRecord> matchOnce(const std::string& streamer_id,
                                         const std::string& expected_public_key);
  void publishFound(const StreamerRecord& rec);
  void searchLoop();

  core::EventBus* bus_;
  std::unique_ptr<IDiscoveryHal> hal_;
  core::ServiceState state_ = core::ServiceState::Stopped;
  bool advertising_ = false;

  // Background search worker.
  std::thread search_thread_;
  mutable std::mutex search_mutex_;
  std::condition_variable search_cv_;
  std::atomic<bool> searching_{false};
  std::string target_id_;
  std::string target_key_;
  std::chrono::milliseconds interval_{std::chrono::seconds(3)};
};

}  // namespace nexus::discovery
