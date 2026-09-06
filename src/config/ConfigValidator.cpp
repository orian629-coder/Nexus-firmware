#include "config/ConfigValidator.h"

namespace nexus::config {

using core::ErrorCode;
using core::Status;

namespace {
Status invalid(std::string msg) {
  return Status::error(ErrorCode::ConfigValidationFailed, std::move(msg));
}
}  // namespace

Status ConfigValidator::validate(const SpeakerConfig& c) {
  if (c.schema_version != 1) {
    return invalid("unsupported schema_version " + std::to_string(c.schema_version));
  }

  // audio
  if (c.audio.volume < 0 || c.audio.volume > 100) {
    return invalid("audio.volume out of range [0,100]: " + std::to_string(c.audio.volume));
  }
  if (c.audio.delay_ms < 0) {
    return invalid("audio.delay_ms must be >= 0: " + std::to_string(c.audio.delay_ms));
  }
  if (c.audio.eq_profile.empty()) {
    return invalid("audio.eq_profile must not be empty");
  }

  // network
  if (c.network.connection_mode != "wifi" && c.network.connection_mode != "ethernet") {
    return invalid("network.connection_mode must be 'wifi' or 'ethernet': " +
                   c.network.connection_mode);
  }

  // system
  const auto& lvl = c.system.log_level;
  if (lvl != "debug" && lvl != "info" && lvl != "warning" && lvl != "error" && lvl != "critical") {
    return invalid("system.log_level invalid: " + lvl);
  }

  // pairing consistency
  if (c.pairing.paired && c.pairing.streamer_id.empty()) {
    return invalid("pairing.paired is true but streamer_id is empty");
  }

  return Status::success();
}

}  // namespace nexus::config
