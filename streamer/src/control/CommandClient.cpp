#include "control/CommandClient.h"

#include "core/ErrorCodes.h"

namespace nexus::streamer::control {

using core::ErrorCode;
using core::Result;
using core::Status;

Result<CommandReply> CommandClient::send(const std::string& command_id,
                                         const std::string& target_id, const std::string& command,
                                         const nlohmann::json& payload, std::int64_t expires_at,
                                         std::int64_t timestamp) {
  auto cmd = signer_.build(command_id, target_id, command, payload, expires_at, timestamp);
  if (!cmd.ok()) return cmd.status();

  const std::string line = cmd.value().toJson().dump();
  auto resp = transport_.request(host_, port_, line);
  if (!resp.ok()) return resp.status();

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(resp.value());
  } catch (const std::exception& e) {
    return Status::error(ErrorCode::InvalidArg, std::string("bad response json: ") + e.what());
  }

  CommandReply reply;
  reply.command_id = j.value("command_id", "");
  reply.ok = j.value("ok", false);
  reply.message = j.value("message", "");
  reply.error_code = j.value("error_code", 0);
  if (j.contains("data") && j["data"].is_object()) reply.data = j["data"];
  return reply;
}

}  // namespace nexus::streamer::control
