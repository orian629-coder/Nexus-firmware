#include "app/CommandGateway.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>

#include <sodium.h>

namespace nexus::streamer::app {

namespace {

std::int64_t defaultNowSeconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t nowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

CommandGateway::CommandGateway(control::ILineTransport& transport, std::string secret_key_b64,
                               std::string streamer_id, Clock clock)
    : transport_(transport),
      secret_key_b64_(std::move(secret_key_b64)),
      streamer_id_(std::move(streamer_id)),
      clock_(clock ? std::move(clock) : Clock(defaultNowSeconds)) {}

// "cmd-<streamer_id>-<epoch_ms>-<counter>-<4 hex>"
//
// epoch_ms + a process-wide atomic counter make collisions impossible within this process even when
// a slider emits many commands per millisecond; streamer_id + randomness cover two streamers and a
// restart landing on the same millisecond. This matters because the speaker caches by command_id
// alone (200-entry LRU) and returns the cached result WITHOUT re-verifying the signature.
std::string CommandGateway::nextCommandId(const std::string& command) {
  const std::uint64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
  std::uint16_t salt = 0;
  randombytes_buf(&salt, sizeof(salt));

  char buf[32];
  std::snprintf(buf, sizeof(buf), "-%llu-%04x", static_cast<unsigned long long>(n), salt);
  return "cmd-" + streamer_id_ + "-" + std::to_string(nowMillis()) + buf + "-" + command;
}

std::mutex& CommandGateway::mutexFor(const std::string& device_id) {
  std::lock_guard<std::mutex> lk(map_mutex_);
  auto it = per_speaker_.find(device_id);
  if (it == per_speaker_.end()) {
    it = per_speaker_.emplace(device_id, std::make_unique<std::mutex>()).first;
  }
  return *it->second;
}

void CommandGateway::setReplyObserver(ReplyObserver observer) {
  std::lock_guard<std::mutex> lk(observer_mutex_);
  observer_ = std::move(observer);
}

core::Result<control::CommandReply> CommandGateway::dispatch(const group::Speaker& target,
                                                             const std::string& command,
                                                             const nlohmann::json& payload,
                                                             int ttl_seconds, bool observe) {
  const std::string command_id = nextCommandId(command);
  const std::int64_t now = clock_();

  core::Result<control::CommandReply> result = [&] {
    // Held only for the duration of this speaker's exchange, so speakers still proceed in parallel.
    std::lock_guard<std::mutex> lk(mutexFor(target.device_id));
    control::CommandClient client(transport_, control::CommandSigner(secret_key_b64_), target.host,
                                  target.control_port);
    return client.send(command_id, target.device_id, command, payload, now + ttl_seconds, now);
  }();

  if (!observe) return result;

  ReplyObserver observer;
  {
    std::lock_guard<std::mutex> lk(observer_mutex_);
    observer = observer_;
  }
  if (observer) {
    static const control::CommandReply kEmpty{};
    observer(target.device_id, result.status(), result.ok() ? result.value() : kEmpty);
  }
  return result;
}

core::Result<control::CommandReply> CommandGateway::send(const group::Speaker& target,
                                                         const std::string& command,
                                                         const nlohmann::json& payload,
                                                         int ttl_seconds) {
  return dispatch(target, command, payload, ttl_seconds, /*observe=*/true);
}

core::Result<control::CommandReply> CommandGateway::sendUnobserved(const group::Speaker& target,
                                                                   const std::string& command,
                                                                   const nlohmann::json& payload,
                                                                   int ttl_seconds) {
  return dispatch(target, command, payload, ttl_seconds, /*observe=*/false);
}

}  // namespace nexus::streamer::app
