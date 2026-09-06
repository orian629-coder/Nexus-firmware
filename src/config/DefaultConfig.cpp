#include "config/DefaultConfig.h"

#include "config/ConfigSerialization.h"

namespace nexus::config {

SpeakerConfig defaults() {
  SpeakerConfig c;  // struct member defaults already encode the safe first-boot values
  c.device.model = "NEXUS-SPEAKER-3";
  c.device.hardware_version = "1.0";
  return c;
}

nlohmann::json defaultJson() { return nlohmann::json(defaults()); }

}  // namespace nexus::config
