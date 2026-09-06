#include "control/CommandRouter.h"

#include <nlohmann/json.hpp>

#include "control/CommandValidator.h"
#include "core/ErrorCodes.h"
#include "logging/Logger.h"

namespace nexus::control {

using core::ErrorCode;

CommandRouter::CommandRouter(CommandExecutor* executor, CommandHistory* history,
                             std::function<std::int64_t()> now_provider)
    : executor_(executor), history_(history), now_(std::move(now_provider)) {}

CommandResult CommandRouter::handle(const std::string& raw_json, const std::string& device_id,
                                    const std::string& streamer_public_key_b64) {
  CommandResult err;
  err.ok = false;

  // Parse.
  Command c;
  try {
    auto j = nlohmann::json::parse(raw_json);
    c = Command::fromJson(j);
  } catch (const std::exception& e) {
    err.message = std::string("malformed command json: ") + e.what();
    err.error_code = core::toInt(ErrorCode::InvalidArg);
    NX_LOG_WARN("control", err.message);
    return err;
  }
  err.command_id = c.command_id;

  // Idempotency: a previously-seen command returns its stored result without re-executing.
  CommandResult prior;
  if (history_ && history_->tryGet(c.command_id, prior)) {
    NX_LOG_INFO("control", "duplicate command [cmd=" + c.command_id + "]; returning cached result");
    return prior;
  }

  // Validate + authenticate (signature).
  core::Status v =
      CommandValidator::validate(c, device_id, streamer_public_key_b64, now_());
  if (!v.ok()) {
    err.command_id = c.command_id;
    err.message = v.message();
    err.error_code = core::toInt(v.code());
    NX_LOG_ERROR("control", v.code(), "rejected command [cmd=" + c.command_id + "]: " + v.message());
    return err;
  }

  // Execute + record.
  CommandResult result = executor_->execute(c);
  // REPORT_LINK is streamer→speaker telemetry that arrives every couple of seconds and changes no
  // device state, so replaying it is harmless and caching it is actively harmful: at that rate it
  // evicts the entire bounded history within minutes, destroying the retry-idempotency of the real
  // commands (SET_VOLUME and friends) the cache exists to protect.
  if (history_ && c.command != cmd::kReportLink) history_->record(c.command_id, result);
  return result;
}

}  // namespace nexus::control
