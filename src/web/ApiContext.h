#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace nexus::web {

// The set of read/action callbacks the API router uses to talk to the rest of the system, injected
// by Application. Keeping this as plain std::functions decouples the web module from every other
// module (no direct dependencies) and makes the router fully testable with fakes.
struct ApiContext {
  // Reads (return JSON).
  std::function<nlohmann::json()> status;      // overall device status
  std::function<nlohmann::json()> health;      // diagnostics report
  std::function<nlohmann::json()> network;     // network info
  std::function<nlohmann::json()> audio;       // audio settings
  std::function<nlohmann::json()> hardware;    // amp/mic/dsp status
  std::function<nlohmann::json()> calibrationStatus;
  std::function<nlohmann::json()> calibrationResult;
  std::function<nlohmann::json()> scanWifi;     // available Wi-Fi networks (for the setup UI)
  std::function<std::string()> logs;           // recent log lines (plain text)

  // Actions (return ok/message). Payload is the parsed request body.
  std::function<std::pair<bool, std::string>(const nlohmann::json&)> setVolume;
  std::function<std::pair<bool, std::string>(const nlohmann::json&)> setMute;
  std::function<std::pair<bool, std::string>(const nlohmann::json&)> setEq;
  std::function<std::pair<bool, std::string>(const nlohmann::json&)> setDelay;
  std::function<std::pair<bool, std::string>()> audioTest;
  std::function<std::pair<bool, std::string>()> startCalibration;
  std::function<std::pair<bool, std::string>()> reboot;
  std::function<std::pair<bool, std::string>()> update;
  std::function<std::pair<bool, std::string>()> resetNetwork;
  std::function<std::pair<bool, std::string>()> factoryReset;
  // Join a Wi-Fi network from the setup UI. Body: {"ssid","psk"} + optional
  // {"hidden","band","static_ip","gateway","dns"}.
  std::function<std::pair<bool, std::string>(const nlohmann::json&)> connectWifi;
  // Set the speaker's display name from the setup UI. Body: {"name": "..."}.
  std::function<std::pair<bool, std::string>(const nlohmann::json&)> setSpeakerName;
};

}  // namespace nexus::web
