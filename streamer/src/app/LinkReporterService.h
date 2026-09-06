#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/CommandGateway.h"
#include "core/IService.h"
#include "core/Result.h"
#include "group/SpeakerRegistry.h"
#include "net/WifiSignal.h"

namespace nexus::streamer::app {

// Pushes this streamer's own Wi-Fi RSSI to every registered speaker as a signed REPORT_LINK.
//
// The speaker's kiosk shows a signal meter for its streamer but cannot measure the streamer's radio
// itself, so the streamer reports it. Best-effort by design: a wired streamer has no RSSI
// (readRssiDbm() == nullopt, which is also the case on macOS since the reader parses
// /proc/net/wireless) and reports nothing, and the speaker falls back to its own RTT-based meter.
// Send failures are ignored here — Phase 2's MonitorService owns reachability, not this.
//
// Behavior is unchanged from the lambda this replaces in StreamerMain; it is a service now only so
// the app can start and stop it in order alongside everything else.
class LinkReporterService : public core::IService {
 public:
  using RssiReader = std::function<std::optional<int>()>;

  LinkReporterService(CommandGateway& gateway, group::SpeakerRegistry& registry,
                      std::string streamer_id, int interval_ms = 2000, RssiReader reader = nullptr)
      : gateway_(gateway),
        registry_(registry),
        streamer_id_(std::move(streamer_id)),
        interval_ms_(interval_ms),
        reader_(reader ? std::move(reader) : RssiReader(&net::WifiSignal::readRssiDbm)) {}

  ~LinkReporterService() override { stop(); }

  std::string name() const override { return "link-reporter"; }

  core::Status start() override {
    if (running_.exchange(true)) return core::Status::success();
    state_ = core::ServiceState::Running;
    thread_ = std::thread([this] { loop(); });
    return core::Status::success();
  }

  core::Status stop() override {
    if (!running_.exchange(false)) return core::Status::success();
    state_ = core::ServiceState::Stopping;
    if (thread_.joinable()) thread_.join();
    state_ = core::ServiceState::Stopped;
    return core::Status::success();
  }

  core::ServiceState state() const override { return state_; }

  std::uint64_t reportsSent() const { return reports_sent_.load(); }

 private:
  void loop() {
    while (running_.load()) {
      const auto rssi = reader_();
      if (rssi.has_value()) {
        for (const auto& sp : registry_.list()) {
          if (!running_.load()) break;
          const nlohmann::json payload{{"wifi_signal_dbm", *rssi}, {"streamer_id", streamer_id_}};
          // Unobserved on purpose: this is telemetry, not a health check. It fires every ~2 s, so
          // letting its failures reach the state store would trip the offline threshold on that
          // clock instead of the 5 s poll's — declaring a speaker dead ~2.5x sooner than configured.
          gateway_.sendUnobserved(sp, "REPORT_LINK", payload);  // best-effort; errors ignored
          reports_sent_.fetch_add(1);
        }
      }
      // Wake often so stop() stays responsive rather than waiting a full interval.
      for (int slept = 0; slept < interval_ms_ && running_.load(); slept += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  CommandGateway& gateway_;
  group::SpeakerRegistry& registry_;
  std::string streamer_id_;
  int interval_ms_;
  RssiReader reader_;

  std::atomic<core::ServiceState> state_{core::ServiceState::Stopped};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> reports_sent_{0};
  std::thread thread_;
};

}  // namespace nexus::streamer::app
