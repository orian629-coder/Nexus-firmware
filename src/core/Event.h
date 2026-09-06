#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace nexus::core {

// The catalog of inter-module events. Modules communicate state changes only through these
// (never direct calls), which is the backbone of fault isolation. Grouped by origin.
enum class EventType {
  // ── lifecycle / system ──
  ServiceStarted,
  ServiceStopped,
  ServiceDegraded,
  ServiceFaulted,
  StateChanged,
  ConfigChanged,
  ConfigInvalid,
  ShutdownRequested,

  // ── identity / pairing ──
  IdentityProvisioned,
  IdentityCorrupt,
  PairingStarted,
  PairingCompleted,
  PairingFailed,

  // ── network / discovery ──
  NetworkConnected,
  NetworkDisconnected,
  IpChanged,
  WifiSignalLow,
  InternetAvailable,
  InternetUnavailable,
  StreamerFound,
  StreamerLost,
  StreamerDisconnected,

  // ── control / audio ──
  CommandReceived,
  CommandExecuted,
  AudioStarted,
  AudioStopped,
  AudioLost,

  // ── hardware / alerts ──
  AmplifierOverheat,
  AmplifierProtection,
  AmplifierFault,
  OutputClipping,
  MicFailure,
  TempWarning,
  HealthCheckFailed,
  WatchdogTimeout,
  SafeModeEntered,

  // ── update / calibration ──
  UpdateAvailable,
  UpdateStarted,
  UpdateFailed,
  UpdateCompleted,
  RollbackTriggered,
  CalibrationStarted,
  CalibrationCompleted,
  CalibrationFailed,
};

const char* toString(EventType t);

// A published event. `data` carries a structured payload but MUST NOT contain secrets, keys,
// or audio. `command_id` is a first-class optional field so command flows are traceable.
struct Event {
  EventType type;
  std::string source;       // module name that published it
  std::string command_id;   // set when the event relates to a command
  nlohmann::json data = nlohmann::json::object();
  std::chrono::system_clock::time_point ts = std::chrono::system_clock::now();

  Event(EventType t, std::string src) : type(t), source(std::move(src)) {}
  Event(EventType t, std::string src, nlohmann::json d)
      : type(t), source(std::move(src)), data(std::move(d)) {}
};

}  // namespace nexus::core
