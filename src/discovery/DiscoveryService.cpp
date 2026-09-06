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

DiscoveryService::~DiscoveryService() { stopSearching(); }

Status DiscoveryService::start() {
  state_ = ServiceState::Running;
  return Status::success();
}

Status DiscoveryService::stop() {
  stopSearching();
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

Result<StreamerRecord> DiscoveryService::matchOnce(const std::string& streamer_id,
                                                   const std::string& expected_public_key) {
  auto found = hal_->browseStreamers();
  if (!found.ok()) return found.status();
  for (const auto& rec : found.value()) {
    if (rec.streamer_id != streamer_id) continue;
    // Trust check: the streamer_id is a public label anyone can advertise. Require the TXT
    // public_key to match the one we stored at pairing before we treat this as our streamer.
    if (!expected_public_key.empty() && rec.public_key != expected_public_key) {
      NX_LOG_WARN("discovery", "streamer " + streamer_id +
                                   " id matched but public key differs — ignoring (possible spoof)");
      continue;
    }
    return rec;
  }
  return Status::error(ErrorCode::NotFound, "streamer not found: " + streamer_id);
}

void DiscoveryService::publishFound(const StreamerRecord& rec) {
  NX_LOG_INFO("discovery", "found streamer " + rec.streamer_id + " at " + rec.host);
  if (bus_) {
    bus_->publish(core::Event{core::EventType::StreamerFound, "discovery",
                              {{"streamer_id", rec.streamer_id},
                               {"host", rec.host},
                               {"port", rec.port}}});
  }
}

Result<StreamerRecord> DiscoveryService::findStreamer(const std::string& streamer_id,
                                                      const std::string& expected_public_key) {
  auto rec = matchOnce(streamer_id, expected_public_key);
  if (rec.ok()) {
    publishFound(rec.value());
    return rec;
  }
  NX_LOG_WARN("discovery", "streamer " + streamer_id + " not found");
  if (bus_) {
    bus_->publish(core::Event{core::EventType::StreamerLost, "discovery",
                              {{"streamer_id", streamer_id}}});
  }
  return rec.status();
}

void DiscoveryService::startSearching(const std::string& streamer_id,
                                      const std::string& expected_public_key,
                                      std::chrono::milliseconds interval) {
  // Restart cleanly so a second call (re-entering SEARCHING_STREAMER, or a new target) never
  // leaves a stale worker running.
  stopSearching();
  {
    std::lock_guard<std::mutex> lk(search_mutex_);
    target_id_ = streamer_id;
    target_key_ = expected_public_key;
    interval_ = interval;
    searching_ = true;
  }
  NX_LOG_INFO("discovery", "searching for streamer " + streamer_id);
  search_thread_ = std::thread([this] { searchLoop(); });
}

void DiscoveryService::stopSearching() {
  {
    std::lock_guard<std::mutex> lk(search_mutex_);
    if (!searching_ && !search_thread_.joinable()) return;
    searching_ = false;
  }
  search_cv_.notify_all();
  if (search_thread_.joinable()) search_thread_.join();
}

void DiscoveryService::searchLoop() {
  // Read the target under the lock once; startSearching() has already joined any prior worker, so
  // these stay stable for this worker's lifetime.
  std::string id, key;
  std::chrono::milliseconds interval;
  {
    std::lock_guard<std::mutex> lk(search_mutex_);
    id = target_id_;
    key = target_key_;
    interval = interval_;
  }

  while (searching_.load()) {
    auto rec = matchOnce(id, key);
    if (rec.ok()) {
      publishFound(rec.value());
      searching_ = false;  // one verified match is enough; SystemManager moves us to Authenticating
      return;
    }
    // Miss: wait out the interval, but wake immediately if asked to stop.
    std::unique_lock<std::mutex> lk(search_mutex_);
    search_cv_.wait_for(lk, interval, [this] { return !searching_.load(); });
  }
}

}  // namespace nexus::discovery
