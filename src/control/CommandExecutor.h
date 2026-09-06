#pragma once

#include <cstdint>
#include <functional>

#include "config/ConfigManager.h"
#include "control/Command.h"
#include "control/LinkState.h"
#include "core/EventBus.h"

namespace nexus::control {

// Executes a validated, deduplicated command and produces a CommandResult. Commands whose target
// modules exist now (volume, mute, delay via config; status; reboot/reset via events) are applied
// directly; commands whose modules land in later phases (audio, DSP, calibration, update) are
// acknowledged and emitted on the bus for those modules to consume as they come online. Either
// way every command returns a structured result — the spec requires "every execution returns a
// result".
class CommandExecutor {
 public:
  // Live playback health, reported in GET_STATUS so the streamer can show why a room sounds wrong
  // rather than only that it is "online".
  //
  // Injected as a std::function returning JSON, NOT as a pointer to the audio module: control must
  // not depend on audio (the CMake link list enforces it), and this keeps the executor testable
  // with a lambda. Unset means this build has no audio pipeline attached, in which case GET_STATUS
  // simply omits the section instead of reporting zeros that would read as a perfectly healthy
  // stream.
  using TelemetryProvider = std::function<nlohmann::json()>;

  // `link_state` (optional) records the streamer's REPORT_LINK telemetry for the status API; `now`
  // (optional) supplies epoch seconds for its freshness timestamp. Both default-safe: if null/empty
  // a REPORT_LINK is still acknowledged, just not recorded.
  CommandExecutor(core::EventBus* bus, config::ConfigManager* config,
                  LinkState* link_state = nullptr, std::function<std::int64_t()> now = nullptr);

  void setTelemetryProvider(TelemetryProvider fn) { telemetry_ = std::move(fn); }

  // Runs an acoustic measurement on this speaker and returns the result as JSON.
  //
  // Injected for the same reason as the telemetry provider: control must not depend on the measure,
  // audio or microphone modules. Unset means this build has no measurement capability, and
  // RUN_MEASUREMENT is refused with a clear reason rather than silently acknowledged — the old
  // "accepted but deferred" behaviour is exactly what made SET_GAIN look like it worked for months.
  //
  // Synchronous and slow by nature (a chirp plus its capture is ~1 s), which is why it is a
  // separate seam the caller can decide to run off the command thread if that ever matters.
  using MeasurementRunner = std::function<nlohmann::json(const nlohmann::json& params)>;
  void setMeasurementRunner(MeasurementRunner fn) { measurement_ = std::move(fn); }

  CommandResult execute(const Command& c);

 private:
  CommandResult ok(const Command& c, nlohmann::json data = nlohmann::json::object());
  CommandResult fail(const Command& c, core::ErrorCode code, const std::string& msg);

  core::EventBus* bus_;
  config::ConfigManager* config_;
  LinkState* link_state_;
  std::function<std::int64_t()> now_;
  TelemetryProvider telemetry_;
  MeasurementRunner measurement_;
};

}  // namespace nexus::control
