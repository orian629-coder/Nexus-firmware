#pragma once

#include <nlohmann/json.hpp>

#include "config/SpeakerConfig.h"

// nlohmann/json (de)serialization for the config structs. Kept separate from SpeakerConfig.h so
// the plain data structs stay dependency-light. Unknown/missing fields fall back to defaults.

namespace nexus::config {

inline void to_json(nlohmann::json& j, const DeviceConfig& d) {
  j = {{"device_id", d.device_id},
       {"serial_number", d.serial_number},
       {"model", d.model},
       {"hardware_version", d.hardware_version},
       {"software_version", d.software_version}};
}
inline void from_json(const nlohmann::json& j, DeviceConfig& d) {
  d.device_id = j.value("device_id", d.device_id);
  d.serial_number = j.value("serial_number", d.serial_number);
  d.model = j.value("model", d.model);
  d.hardware_version = j.value("hardware_version", d.hardware_version);
  d.software_version = j.value("software_version", d.software_version);
}

inline void to_json(nlohmann::json& j, const PairingConfig& p) {
  j = {{"paired", p.paired},
       {"streamer_id", p.streamer_id},
       {"streamer_public_key", p.streamer_public_key}};
}
inline void from_json(const nlohmann::json& j, PairingConfig& p) {
  p.paired = j.value("paired", p.paired);
  p.streamer_id = j.value("streamer_id", p.streamer_id);
  p.streamer_public_key = j.value("streamer_public_key", p.streamer_public_key);
}

inline void to_json(nlohmann::json& j, const NetworkConfig& n) {
  j = {{"wifi_configured", n.wifi_configured}, {"connection_mode", n.connection_mode}};
}
inline void from_json(const nlohmann::json& j, NetworkConfig& n) {
  n.wifi_configured = j.value("wifi_configured", n.wifi_configured);
  n.connection_mode = j.value("connection_mode", n.connection_mode);
}

inline void to_json(nlohmann::json& j, const AudioConfig& a) {
  j = {{"volume", a.volume},
       {"muted", a.muted},
       {"delay_ms", a.delay_ms},
       {"eq_profile", a.eq_profile},
       {"gain_db", a.gain_db},
       {"phase_invert", a.phase_invert},
       {"output_device", a.output_device},
       {"input_device", a.input_device}};
}
inline void from_json(const nlohmann::json& j, AudioConfig& a) {
  a.volume = j.value("volume", a.volume);
  a.muted = j.value("muted", a.muted);
  a.delay_ms = j.value("delay_ms", a.delay_ms);
  a.eq_profile = j.value("eq_profile", a.eq_profile);
  a.gain_db = j.value("gain_db", a.gain_db);
  a.phase_invert = j.value("phase_invert", a.phase_invert);
  a.output_device = j.value("output_device", a.output_device);
  a.input_device = j.value("input_device", a.input_device);
}

inline void to_json(nlohmann::json& j, const SystemConfig& s) {
  j = {{"auto_update", s.auto_update}, {"log_level", s.log_level}};
}
inline void from_json(const nlohmann::json& j, SystemConfig& s) {
  s.auto_update = j.value("auto_update", s.auto_update);
  s.log_level = j.value("log_level", s.log_level);
}

inline void to_json(nlohmann::json& j, const SpeakerConfig& c) {
  j = {{"schema_version", c.schema_version},
       {"device", c.device},
       {"pairing", c.pairing},
       {"network", c.network},
       {"audio", c.audio},
       {"system", c.system}};
}
inline void from_json(const nlohmann::json& j, SpeakerConfig& c) {
  c.schema_version = j.value("schema_version", c.schema_version);
  if (j.contains("device")) c.device = j.at("device").get<DeviceConfig>();
  if (j.contains("pairing")) c.pairing = j.at("pairing").get<PairingConfig>();
  if (j.contains("network")) c.network = j.at("network").get<NetworkConfig>();
  if (j.contains("audio")) c.audio = j.at("audio").get<AudioConfig>();
  if (j.contains("system")) c.system = j.at("system").get<SystemConfig>();
}

}  // namespace nexus::config
