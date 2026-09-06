#pragma once

#include <nlohmann/json.hpp>

#include "config/SpeakerConfig.h"

namespace nexus::config {

// The seed configuration used on first boot (UNCONFIGURED) and as the reference the shipped
// config/default.json is validated against. `defaults()` is the single source of truth.
SpeakerConfig defaults();
nlohmann::json defaultJson();

}  // namespace nexus::config
