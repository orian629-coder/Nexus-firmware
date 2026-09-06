#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "config/ConfigManager.h"
#include "control/CommandExecutor.h"
#include "control/CommandHistory.h"
#include "control/CommandRouter.h"
#include "control/ICommandTransport.h"
#include "control/LinkState.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "identity/DeviceIdentity.h"

namespace nexus::control {

// The signed command server. Listens on TCP 45455 (via ICommandTransport), and for each request
// runs the CommandRouter pipeline (validate → authenticate → dedup → execute → record) and returns
// the JSON result. The Streamer is the sole command source; commands must be signed by the paired
// streamer's key (from config) or they are rejected.
class CommandServer : public core::IService {
 public:
  static constexpr int kDefaultPort = 45455;

  CommandServer(core::EventBus* bus, identity::DeviceIdentity* identity,
                config::ConfigManager* config,
                std::function<std::int64_t()> now_provider,
                std::unique_ptr<ICommandTransport> transport = nullptr, int port = kDefaultPort);

  std::string name() const override { return "control"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Optional handler for inbound pairing requests (JSON with type == "pairing"). Injected by
  // Application so the control module stays decoupled from the pairing module. Takes the raw JSON
  // and returns the JSON response.
  using PairingHandler = std::function<std::string(const std::string& raw_json)>;
  void setPairingHandler(PairingHandler h) { pairing_handler_ = std::move(h); }

  // Playback health reported inside GET_STATUS. Injected by Application (which owns the audio
  // pipeline) for the same reason as the pairing handler: control stays decoupled from audio.
  void setTelemetryProvider(CommandExecutor::TelemetryProvider fn) {
    executor_->setTelemetryProvider(std::move(fn));
  }

  // Acoustic measurement, injected by Application (which owns the audio output and the microphone).
  void setMeasurementRunner(CommandExecutor::MeasurementRunner fn) {
    executor_->setMeasurementRunner(std::move(fn));
  }

  // Handle one raw request (exposed for tests and reused by the transport).
  std::string handleRaw(const std::string& raw_json);

  // Access the transport (e.g. StubCommandTransport::deliver in tests).
  ICommandTransport* transport() { return transport_.get(); }

  // Live streamer link telemetry (last REPORT_LINK). The web/status layer reads this to drive the
  // kiosk's per-streamer signal meter. Stable for the server's lifetime.
  LinkState* linkState() { return &link_state_; }

 private:
  core::EventBus* bus_;
  identity::DeviceIdentity* identity_;
  config::ConfigManager* config_;
  int port_;

  std::unique_ptr<ICommandTransport> transport_;
  CommandHistory history_;
  LinkState link_state_;  // declared before executor_ so it outlives the executor that writes it
  std::unique_ptr<CommandExecutor> executor_;
  std::unique_ptr<CommandRouter> router_;
  PairingHandler pairing_handler_;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::control
