#include "control/CommandServer.h"

#include <nlohmann/json.hpp>

#include "logging/Logger.h"

#if NEXUS_STUB_HAL
// StubCommandTransport is defined in ICommandTransport.h.
#else
#include "control/TcpCommandTransport.h"
#endif

namespace nexus::control {

using core::ServiceState;
using core::Status;

namespace {
std::unique_ptr<ICommandTransport> makeDefaultTransport() {
#if NEXUS_STUB_HAL
  return std::make_unique<StubCommandTransport>();
#else
  return std::make_unique<TcpCommandTransport>();
#endif
}
}  // namespace

CommandServer::CommandServer(core::EventBus* bus, identity::DeviceIdentity* identity,
                             config::ConfigManager* config,
                             std::function<std::int64_t()> now_provider,
                             std::unique_ptr<ICommandTransport> transport, int port)
    : bus_(bus),
      identity_(identity),
      config_(config),
      port_(port),
      transport_(transport ? std::move(transport) : makeDefaultTransport()) {
  // The executor records REPORT_LINK telemetry into link_state_ with a timestamp; it shares the
  // same clock as the router's freshness/dedup logic. Copy the provider before moving it so both
  // get a valid clock.
  auto clock = now_provider;
  executor_ = std::make_unique<CommandExecutor>(bus_, config_, &link_state_, clock);
  router_ = std::make_unique<CommandRouter>(executor_.get(), &history_, std::move(now_provider));
}

std::string CommandServer::handleRaw(const std::string& raw_json) {
  // A pairing request (type == "pairing") is handled by the pairing module — it isn't signed by a
  // paired streamer yet, so it bypasses the normal command pipeline and uses the setup-code +
  // request-signature validation inside PairingService.
  if (pairing_handler_) {
    try {
      auto j = nlohmann::json::parse(raw_json);
      if (j.value("type", "") == "pairing") return pairing_handler_(raw_json);
    } catch (...) {
      // fall through to the command pipeline, which returns a proper malformed-json error
    }
  }

  const std::string device_id = identity_ ? identity_->deviceId() : std::string();
  const std::string streamer_key = config_ ? config_->get().pairing.streamer_public_key
                                           : std::string();
  CommandResult r = router_->handle(raw_json, device_id, streamer_key);
  return r.toJson().dump();
}

Status CommandServer::start() {
  Status s = transport_->start(port_, [this](const std::string& req) { return handleRaw(req); });
  state_ = s.ok() ? ServiceState::Running : ServiceState::Degraded;
  if (!s.ok()) {
    NX_LOG_ERROR("control", s.code(), "command transport failed to start: " + s.message());
  }
  return s;
}

Status CommandServer::stop() {
  if (transport_) transport_->stop();
  state_ = ServiceState::Stopped;
  return Status::success();
}

}  // namespace nexus::control
