#pragma once

#include <vector>

#include <nlohmann/json.hpp>

#include "config/StreamerConfig.h"

namespace nexus::streamer::config {

// JSON (de)serialization for the streamer's on-disk config, following the speaker's
// config/ConfigSerialization.h conventions.
//
// Every read goes through j.value(key, default), so a config written by an older build — or one
// hand-edited and missing a section — still loads with sane values instead of throwing. That is what
// makes adding a field a non-breaking change.

inline void to_json(nlohmann::json& j, const IdentityConfig& c) {
  j = {{"streamer_id", c.streamer_id}, {"public_key", c.public_key}, {"site_id", c.site_id}};
}
inline void from_json(const nlohmann::json& j, IdentityConfig& c) {
  c.streamer_id = j.value("streamer_id", c.streamer_id);
  c.public_key = j.value("public_key", c.public_key);
  c.site_id = j.value("site_id", c.site_id);
}

inline void to_json(nlohmann::json& j, const SpeakerRecord& s) {
  j = {{"device_id", s.device_id},
       {"name", s.name},
       {"host", s.host},
       {"control_port", s.control_port},
       {"public_key", s.public_key},
       {"box_public_key", s.box_public_key},
       {"delay_ms", s.delay_ms}};
}
inline void from_json(const nlohmann::json& j, SpeakerRecord& s) {
  s.device_id = j.value("device_id", s.device_id);
  s.name = j.value("name", s.name);
  s.host = j.value("host", s.host);
  s.control_port = j.value("control_port", s.control_port);
  s.public_key = j.value("public_key", s.public_key);
  s.box_public_key = j.value("box_public_key", s.box_public_key);
  s.delay_ms = j.value("delay_ms", s.delay_ms);
}

inline void to_json(nlohmann::json& j, const ZoneRecord& z) {
  j = {{"zone_id", z.zone_id}, {"name", z.name}, {"members", z.members}};
}
inline void from_json(const nlohmann::json& j, ZoneRecord& z) {
  z.zone_id = j.value("zone_id", z.zone_id);
  z.name = j.value("name", z.name);
  z.members = j.value("members", z.members);
}

inline void to_json(nlohmann::json& j, const AudioConfig& a) {
  j = {{"sample_rate", a.sample_rate},
       {"channels", a.channels},
       {"block_frames", a.block_frames},
       {"lead_ms", a.lead_ms},
       {"source", a.source}};
}
inline void from_json(const nlohmann::json& j, AudioConfig& a) {
  a.sample_rate = j.value("sample_rate", a.sample_rate);
  a.channels = j.value("channels", a.channels);
  a.block_frames = j.value("block_frames", a.block_frames);
  a.lead_ms = j.value("lead_ms", a.lead_ms);
  a.source = j.value("source", a.source);
}

inline void to_json(nlohmann::json& j, const NetworkConfig& n) {
  j = {{"control_port", n.control_port}, {"audio_port", n.audio_port}};
}
inline void from_json(const nlohmann::json& j, NetworkConfig& n) {
  n.control_port = j.value("control_port", n.control_port);
  n.audio_port = j.value("audio_port", n.audio_port);
}

inline void to_json(nlohmann::json& j, const WebConfig& w) {
  j = {{"port", w.port}, {"auth_token", w.auth_token}, {"bind_address", w.bind_address}};
}
inline void from_json(const nlohmann::json& j, WebConfig& w) {
  w.port = j.value("port", w.port);
  w.auth_token = j.value("auth_token", w.auth_token);
  w.bind_address = j.value("bind_address", w.bind_address);
}

inline void to_json(nlohmann::json& j, const SystemConfig& s) {
  j = {{"log_level", s.log_level}};
}
inline void from_json(const nlohmann::json& j, SystemConfig& s) {
  s.log_level = j.value("log_level", s.log_level);
}

inline void to_json(nlohmann::json& j, const StreamerConfig& c) {
  j = {{"schema_version", c.schema_version},
       {"identity", c.identity},
       {"speakers", c.speakers},
       {"zones", c.zones},
       {"audio", c.audio},
       {"dsp", c.dsp},
       {"network", c.network},
       {"web", c.web},
       {"system", c.system}};
}
inline void from_json(const nlohmann::json& j, StreamerConfig& c) {
  c.schema_version = j.value("schema_version", c.schema_version);
  if (j.contains("identity")) c.identity = j.at("identity").get<IdentityConfig>();
  if (j.contains("speakers")) c.speakers = j.at("speakers").get<std::vector<SpeakerRecord>>();
  if (j.contains("zones")) c.zones = j.at("zones").get<std::vector<ZoneRecord>>();
  if (j.contains("audio")) c.audio = j.at("audio").get<AudioConfig>();
  // Opaque passthrough: only an object is accepted, so a corrupt scalar leaves defaults in place
  // rather than propagating a value MasterDspConfig would have to defend against.
  if (j.contains("dsp") && j.at("dsp").is_object()) c.dsp = j.at("dsp");
  if (j.contains("network")) c.network = j.at("network").get<NetworkConfig>();
  if (j.contains("web")) c.web = j.at("web").get<WebConfig>();
  if (j.contains("system")) c.system = j.at("system").get<SystemConfig>();
}

}  // namespace nexus::streamer::config
