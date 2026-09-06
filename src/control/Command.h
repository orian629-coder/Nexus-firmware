#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace nexus::control {

// A command received from the Streamer. Wire format (JSON):
//   {
//     "type": "command",
//     "command_id": "cmd-12345",
//     "target_id": "SPK-A104",
//     "command": "SET_VOLUME",
//     "payload": { ... },
//     "expires_at": 1785100300,
//     "timestamp": 1785100000,
//     "signature": "<base64 Ed25519 over canonicalString()>"
//   }
// The signature is made by the Streamer's private key and verified against the paired streamer
// public key stored in config. The canonical string excludes the signature and fixes field order.
struct Command {
  std::string command_id;
  std::string target_id;
  std::string command;              // e.g. "SET_VOLUME"
  nlohmann::json payload = nlohmann::json::object();
  std::int64_t expires_at = 0;
  std::int64_t timestamp = 0;
  std::string signature_b64;

  // Bytes the signature is computed over. Payload is serialized compactly with sorted keys so both
  // sides agree byte-for-byte.
  std::string canonicalString() const {
    return command_id + "\n" + target_id + "\n" + command + "\n" +
           payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict) + "\n" +
           std::to_string(expires_at) + "\n" + std::to_string(timestamp);
  }

  static Command fromJson(const nlohmann::json& j);
  nlohmann::json toJson() const;
};

// The result returned to the Streamer for each command.
struct CommandResult {
  std::string command_id;
  bool ok = false;
  std::string message;
  int error_code = 0;
  nlohmann::json data = nlohmann::json::object();

  nlohmann::json toJson() const {
    return {{"type", "command_result"}, {"command_id", command_id}, {"ok", ok},
            {"message", message},        {"error_code", error_code}, {"data", data}};
  }
};

// The mandatory command set from the specification.
namespace cmd {
constexpr const char* kGetStatus = "GET_STATUS";
constexpr const char* kSetVolume = "SET_VOLUME";
constexpr const char* kSetMute = "SET_MUTE";
constexpr const char* kSetEq = "SET_EQ";
constexpr const char* kSetDelay = "SET_DELAY";
constexpr const char* kSetGain = "SET_GAIN";
// Per-speaker polarity flip. Separate from SET_GAIN because it is not a level at all: it changes
// the sign of the samples, and a room fix that needs it cannot be expressed as any amount of gain.
constexpr const char* kSetPhaseInvert = "SET_PHASE_INVERT";
constexpr const char* kStartAudio = "START_AUDIO";
constexpr const char* kStopAudio = "STOP_AUDIO";
constexpr const char* kPauseAudio = "PAUSE_AUDIO";
constexpr const char* kResumeAudio = "RESUME_AUDIO";
constexpr const char* kRunCalibration = "RUN_CALIBRATION";
// Acoustic distance measurement: this speaker emits a chirp from its own output and reports what
// each of its microphones heard. Play and record must happen on the SAME device, so the streamer
// asks rather than does — it has no way to know when the sound actually left the driver.
constexpr const char* kRunMeasurement = "RUN_MEASUREMENT";
constexpr const char* kRunSelfTest = "RUN_SELF_TEST";
constexpr const char* kRunAudioTest = "RUN_AUDIO_TEST";
constexpr const char* kReboot = "REBOOT";
constexpr const char* kUpdateSoftware = "UPDATE_SOFTWARE";
constexpr const char* kResetNetwork = "RESET_NETWORK";
constexpr const char* kFactoryReset = "FACTORY_RESET";
// The streamer periodically reports its own Wi-Fi link strength so the speaker's kiosk can show a
// per-device signal meter for the connected streamer. Payload: {"wifi_signal_dbm": int (negative),
// "streamer_id": string}. Not in the mandatory spec set — it's a telemetry push, acknowledged and
// recorded, never persisted.
constexpr const char* kReportLink = "REPORT_LINK";
}  // namespace cmd

}  // namespace nexus::control
