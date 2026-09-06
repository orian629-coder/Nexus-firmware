#include "control/CommandSigner.h"

#include "identity/Crypto.h"

namespace nexus::streamer::control {

using core::Result;
using nexus::control::Command;

Result<Command> CommandSigner::build(const std::string& command_id, const std::string& target_id,
                                     const std::string& command, const nlohmann::json& payload,
                                     std::int64_t expires_at, std::int64_t timestamp) const {
  Command c;
  c.command_id = command_id;
  c.target_id = target_id;
  c.command = command;
  // The speaker's Command::fromJson keeps payload only if it is a JSON object, otherwise it stays
  // the default empty object. A null/absent payload (e.g. nlohmann `{}`, which is null) would sign a
  // canonical string the speaker never reconstructs → signature mismatch. Normalize to {} so both
  // sides serialize the SAME bytes.
  c.payload = payload.is_object() ? payload : nlohmann::json::object();
  c.expires_at = expires_at;
  c.timestamp = timestamp;

  // Sign the raw canonical bytes; canonicalString() serializes payload compactly with sorted keys,
  // matching the speaker byte-for-byte.
  auto sig = nexus::identity::crypto::signEd25519Base64(c.canonicalString(), secret_key_b64_);
  if (!sig.ok()) return sig.status();
  c.signature_b64 = sig.value();
  return c;
}

}  // namespace nexus::streamer::control
