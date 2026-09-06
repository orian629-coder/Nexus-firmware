#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "control/CommandSigner.h"
#include "control/ILineTransport.h"
#include "core/Result.h"

namespace nexus::streamer::control {

// Parsed command_result from the speaker (mirror of control::CommandResult on the wire).
struct CommandReply {
  std::string command_id;
  bool ok = false;
  std::string message;
  int error_code = 0;
  nlohmann::json data = nlohmann::json::object();
};

// Sends signed commands to a speaker's control endpoint and returns the parsed reply. It composes
// CommandSigner (shared canonical-string signing) with an ILineTransport (fresh TCP dial per
// command, matching the speaker's request-per-connection server). command_id/expires_at/timestamp
// are caller-supplied so the clock and id source stay injectable and testable; retries reuse the
// same command_id for the speaker's idempotency cache.
class CommandClient {
 public:
  CommandClient(ILineTransport& transport, CommandSigner signer, std::string host, int port)
      : transport_(transport), signer_(std::move(signer)), host_(std::move(host)), port_(port) {}

  core::Result<CommandReply> send(const std::string& command_id, const std::string& target_id,
                                  const std::string& command, const nlohmann::json& payload,
                                  std::int64_t expires_at, std::int64_t timestamp);

 private:
  ILineTransport& transport_;
  CommandSigner signer_;
  std::string host_;
  int port_;
};

}  // namespace nexus::streamer::control
