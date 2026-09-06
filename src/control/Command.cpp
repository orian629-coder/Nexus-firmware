#include "control/Command.h"

namespace nexus::control {

Command Command::fromJson(const nlohmann::json& j) {
  Command c;
  c.command_id = j.value("command_id", "");
  c.target_id = j.value("target_id", "");
  c.command = j.value("command", "");
  if (j.contains("payload") && j.at("payload").is_object()) c.payload = j.at("payload");
  c.expires_at = j.value("expires_at", static_cast<std::int64_t>(0));
  c.timestamp = j.value("timestamp", static_cast<std::int64_t>(0));
  c.signature_b64 = j.value("signature", "");
  return c;
}

nlohmann::json Command::toJson() const {
  return {{"type", "command"}, {"command_id", command_id}, {"target_id", target_id},
          {"command", command}, {"payload", payload},       {"expires_at", expires_at},
          {"timestamp", timestamp}, {"signature", signature_b64}};
}

}  // namespace nexus::control
