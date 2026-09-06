#pragma once

#include <string>

namespace nexus::config {

// Typed mirror of the on-disk config schema (config/default.json). Contains NO secrets:
// Wi-Fi passwords and private keys live in SecureStorage, never here. `streamer_public_key`
// is public and safe to store. All fields are defaulted so a partial/first-boot config still
// yields a usable object.

struct DeviceConfig {
  std::string device_id;         // mirror of factory identity; informational only
  std::string name;              // user-facing display name (how it shows to the streamer); set in setup
  std::string serial_number;
  std::string model;
  std::string hardware_version;
  std::string software_version = "1.0.0";
};

struct PairingConfig {
  bool paired = false;
  std::string streamer_id;
  std::string streamer_public_key;  // public key — safe to persist in config
};

struct NetworkConfig {
  bool wifi_configured = false;         // flag only; credentials are in SecureStorage
  std::string connection_mode = "wifi"; // "wifi" | "ethernet"
};

struct AudioConfig {
  int volume = 30;         // 0..100; conservative default at first boot
  bool muted = true;       // stay muted until amplifier is confirmed stable
  int delay_ms = 0;        // >= 0
  std::string eq_profile = "default";
  // Per-speaker trim in dB, applied in the DSP output stage. Distinct from `volume`, which is the
  // listener's 0..100 control: this is the installer's channel-matching adjustment, so a speaker
  // further from the listening position can be level-matched once and stay matched at every volume
  // setting. Clamped to +/-20 dB — beyond that the fix is speaker placement, not gain.
  double gain_db = 0.0;
  // Polarity inversion. A driver wired backwards, or one positioned such that its output arrives
  // out of phase, cancels bass against its neighbours; flipping the sign of its samples fixes it.
  // Applied per speaker, so it must live here rather than in the streamer's shared master chain.
  bool phase_invert = false;
  // ALSA device for playback (e.g. "hw:0,0", "plughw:CARD=Headphones,DEV=0").
  //
  // WARNING: "default" is not always a real card. On a Pi with no I2S HAT and no ~/.asoundrc,
  // `aplay -L` reports default -> null ("Discard all samples") — playback then succeeds at every
  // level (packets decode, the pipeline runs, no error is logged) while the audio is silently
  // thrown away. Check `aplay -L | head -3` on the device; if default maps to null, set an explicit
  // card here. Verify with /proc/asound/card<N>/pcm0p/sub0/status: state must be RUNNING and hw_ptr
  // must advance by ~48000 per second.
  std::string output_device = "default";
  std::string input_device = "default";   // ALSA device for the measurement mic
};

struct SystemConfig {
  bool auto_update = true;
  std::string log_level = "info";  // debug|info|warning|error|critical
};

struct SpeakerConfig {
  int schema_version = 1;
  DeviceConfig device;
  PairingConfig pairing;
  NetworkConfig network;
  AudioConfig audio;
  SystemConfig system;
};

}  // namespace nexus::config
