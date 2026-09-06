#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "control/Command.h"
#include "control/CommandExecutor.h"
#include "control/CommandHistory.h"

namespace nexus::control {

// The command pipeline core (pure, no transport):
//   Validate → Authenticate (signature) → Check Command ID (idempotency) → Execute → Store History
// Given raw JSON bytes and the current identity/pairing context, it returns the CommandResult to
// send back. A duplicate command_id returns the stored result without re-executing.
class CommandRouter {
 public:
  // `now_provider` supplies the current epoch time (injected so tests are deterministic and the
  // module avoids the banned argless clock calls).
  CommandRouter(CommandExecutor* executor, CommandHistory* history,
                std::function<std::int64_t()> now_provider);

  // Process a raw JSON command payload. `device_id` and `streamer_public_key_b64` come from
  // identity/config. Never throws — malformed input yields an error CommandResult.
  CommandResult handle(const std::string& raw_json, const std::string& device_id,
                       const std::string& streamer_public_key_b64);

 private:
  CommandExecutor* executor_;
  CommandHistory* history_;
  std::function<std::int64_t()> now_;
};

}  // namespace nexus::control
