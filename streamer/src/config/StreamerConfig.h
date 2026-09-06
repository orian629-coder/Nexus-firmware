#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace nexus::streamer::config {

// Typed mirror of the streamer's on-disk config schema (config/streamer.default.json). Contains NO
// secrets: the streamer's Ed25519 private key lives in SecureStorage, never here. Public keys and
// speaker records are safe to persist. All fields are defaulted so a partial/first-boot config
// still yields a usable object. Mirrors the speaker's config/SpeakerConfig.h conventions.

struct IdentityConfig {
  std::string streamer_id;    // e.g. "STR-LAB01"; advertised in mDNS and used as target/site key
  std::string public_key;     // base64 Ed25519 public key — safe to persist; sent during pairing
  std::string site_id;        // logical site/home this streamer manages
};

// One paired speaker the streamer sends audio + commands to. Part of a zone's fan-out set.
//
// Addressing only — nothing the speaker reports about itself is persisted. Volume, mute, EQ and
// reachability are owned by the speaker and mirrored at runtime in state::SpeakerStateStore; writing
// them here would recreate the stale-value problem that store exists to prevent.
struct SpeakerRecord {
  std::string device_id;         // speaker identity (SPK-XXXX); becomes command target_id
  std::string name;              // human label ("Living Room")
  std::string host;              // resolved IPv4 / hostname for control + audio
  int control_port = 45455;      // speaker command/pairing TCP port
  std::string public_key;        // base64 Ed25519; verifies nothing here but kept for parity
  std::string box_public_key;    // base64 X25519; used to seal Wi-Fi creds during (re)pairing
  int delay_ms = 0;              // per-room fixed offset tuning (SET_DELAY)
};

// A named set of speakers that plays one synchronized stream. Purely a streamer-side concept: the
// speaker firmware has no notion of zones and is always addressed individually by device_id.
struct ZoneRecord {
  std::string zone_id;                 // stable generated id ("zone-xxxxxxxx")
  std::string name;                    // human label ("קומה ראשונה")
  std::vector<std::string> members;    // device_ids
};

struct AudioConfig {
  int sample_rate = 48000;   // fixed by the wire contract
  int channels = 2;
  int block_frames = 480;    // 10 ms @ 48k; payload = block_frames*channels int16
  int lead_ms = 180;         // future-stamp lead: >= StreamSync target (~80ms) + WiFi jitter margin
  std::string source = "line-in";  // active source: line-in|bluetooth|airplay|spotify|file
};

struct NetworkConfig {
  std::string control_port = "45455";   // speaker command/pairing TCP port (contract default)
  int audio_port = 50005;               // speaker UDP audio port (contract default)
};

struct WebConfig {
  int port = 8090;
  // Bearer token for the control API, generated on first run. This is the ONE secret in this file;
  // it is not a device key and can be rotated by deleting it. Persisted so the browser does not have
  // to be re-authorized on every restart.
  std::string auth_token;
  // Bind address. Defaults to loopback: the API can pair speakers, which carries a Wi-Fi PSK, so
  // exposing it to the whole LAN is opt-in via --bind rather than the default.
  std::string bind_address = "127.0.0.1";
};

struct SystemConfig {
  std::string log_level = "info";  // debug|info|warning|error|critical
};

struct StreamerConfig {
  int schema_version = 1;
  IdentityConfig identity;
  std::vector<SpeakerRecord> speakers;  // paired speakers
  std::vector<ZoneRecord> zones;        // named fan-out sets over those speakers
  AudioConfig audio;
  // Master DSP chain settings (bypass/gains/EQ curve), stored opaquely as JSON so the audio-side
  // schema can evolve without touching the config layer. Serialized/parsed by
  // dsp::MasterDspConfig; an empty object means "defaults" (flat EQ, 0 dB, limiter on).
  nlohmann::json dsp = nlohmann::json::object();
  NetworkConfig network;
  WebConfig web;
  SystemConfig system;
};

}  // namespace nexus::streamer::config
