#pragma once

#include "config/SpeakerConfig.h"
#include "core/Result.h"

namespace nexus::config {

// Pure validation of a SpeakerConfig. Runs on every load AND before every save, so an invalid
// config is never persisted. Returns the first violation found, with an ErrorCode and a
// human-readable message.
class ConfigValidator {
 public:
  static core::Status validate(const SpeakerConfig& c);
};

}  // namespace nexus::config
