#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "control/CommandClient.h"
#include "control/CommandSigner.h"
#include "control/ILineTransport.h"
#include "core/Result.h"
#include "group/SpeakerRegistry.h"

namespace nexus::streamer::app {

// The ONE place a command is built, signed, and sent to a speaker.
//
// Everything that talks to a speaker (web router, link reporter, and the Phase-2 monitor) goes
// through here, which is what makes two guarantees enforceable in a single spot:
//
//  1. command_id uniqueness. The speaker's CommandRouter checks its idempotency cache BEFORE
//     verifying the signature and returns the cached result for a repeated id, so a colliding id
//     silently reports a stale value instead of executing. The previous id was
//     "cmd-<epoch_seconds>-<command>" — one-second granularity, which a dragged volume slider
//     collides with several times per second. See nextCommandId().
//  2. Per-speaker serialization. The control channel dials a fresh TCP connection per command with
//     a 3 s socket timeout; a status poll and a user action racing on the same speaker produce
//     spurious timeouts that read as "offline". One mutex per device_id keeps them ordered.
//
// Phase 2 adds the confirm hook: every reply is funneled into SpeakerStateStore here, so the UI can
// never show a value the speaker did not report.
class CommandGateway {
 public:
  // Observes every completed exchange. `reply` is only meaningful when `status.ok()`. Phase 2 wires
  // SpeakerStateStore to this; Phase 1 leaves it null.
  using ReplyObserver = std::function<void(const std::string& device_id, const core::Status& status,
                                           const control::CommandReply& reply)>;

  // `clock` returns epoch seconds; injected so tests stay deterministic (same pattern as the
  // speaker's StreamSync/StatusService).
  using Clock = std::function<std::int64_t()>;

  CommandGateway(control::ILineTransport& transport, std::string secret_key_b64,
                 std::string streamer_id, Clock clock = nullptr);

  // Sign and send one command, blocking until the speaker replies or the transport times out.
  // Serialized per target.device_id. Never throws.
  core::Result<control::CommandReply> send(const group::Speaker& target, const std::string& command,
                                           const nlohmann::json& payload = nlohmann::json::object(),
                                           int ttl_seconds = 30);

  // Same, but the outcome is NOT reported to the observer, so it neither confirms state nor counts
  // toward offline detection.
  //
  // This exists for best-effort telemetry (REPORT_LINK), which fires far more often than the status
  // poll. Routing it through the normal path would let its failures drive the offline threshold on a
  // much shorter clock than the poll interval, so a speaker would be declared dead several times
  // sooner than configured. Reachability must be decided by the poll that is actually asking, and by
  // nothing else.
  core::Result<control::CommandReply> sendUnobserved(
      const group::Speaker& target, const std::string& command,
      const nlohmann::json& payload = nlohmann::json::object(), int ttl_seconds = 30);

  void setReplyObserver(ReplyObserver observer);

  // Exposed for the uniqueness regression test.
  std::string nextCommandId(const std::string& command);

 private:
  core::Result<control::CommandReply> dispatch(const group::Speaker& target,
                                               const std::string& command,
                                               const nlohmann::json& payload, int ttl_seconds,
                                               bool observe);
  std::mutex& mutexFor(const std::string& device_id);

  control::ILineTransport& transport_;
  std::string secret_key_b64_;
  std::string streamer_id_;
  Clock clock_;

  std::atomic<std::uint64_t> counter_{0};

  mutable std::mutex map_mutex_;
  std::map<std::string, std::unique_ptr<std::mutex>> per_speaker_;

  mutable std::mutex observer_mutex_;
  ReplyObserver observer_;
};

}  // namespace nexus::streamer::app
