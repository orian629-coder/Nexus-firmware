#include "core/EventBus.h"

#include <cstdio>

namespace nexus::core {

const char* toString(EventType t) {
  switch (t) {
    case EventType::ServiceStarted: return "ServiceStarted";
    case EventType::ServiceStopped: return "ServiceStopped";
    case EventType::ServiceDegraded: return "ServiceDegraded";
    case EventType::ServiceFaulted: return "ServiceFaulted";
    case EventType::StateChanged: return "StateChanged";
    case EventType::ConfigChanged: return "ConfigChanged";
    case EventType::ConfigInvalid: return "ConfigInvalid";
    case EventType::ShutdownRequested: return "ShutdownRequested";
    case EventType::IdentityProvisioned: return "IdentityProvisioned";
    case EventType::IdentityCorrupt: return "IdentityCorrupt";
    case EventType::PairingStarted: return "PairingStarted";
    case EventType::PairingCompleted: return "PairingCompleted";
    case EventType::PairingFailed: return "PairingFailed";
    case EventType::NetworkConnected: return "NetworkConnected";
    case EventType::NetworkDisconnected: return "NetworkDisconnected";
    case EventType::IpChanged: return "IpChanged";
    case EventType::WifiSignalLow: return "WifiSignalLow";
    case EventType::InternetAvailable: return "InternetAvailable";
    case EventType::InternetUnavailable: return "InternetUnavailable";
    case EventType::StreamerFound: return "StreamerFound";
    case EventType::StreamerLost: return "StreamerLost";
    case EventType::StreamerDisconnected: return "StreamerDisconnected";
    case EventType::CommandReceived: return "CommandReceived";
    case EventType::CommandExecuted: return "CommandExecuted";
    case EventType::AudioStarted: return "AudioStarted";
    case EventType::AudioStopped: return "AudioStopped";
    case EventType::AudioLost: return "AudioLost";
    case EventType::AmplifierOverheat: return "AmplifierOverheat";
    case EventType::AmplifierProtection: return "AmplifierProtection";
    case EventType::AmplifierFault: return "AmplifierFault";
    case EventType::OutputClipping: return "OutputClipping";
    case EventType::MicFailure: return "MicFailure";
    case EventType::TempWarning: return "TempWarning";
    case EventType::HealthCheckFailed: return "HealthCheckFailed";
    case EventType::WatchdogTimeout: return "WatchdogTimeout";
    case EventType::SafeModeEntered: return "SafeModeEntered";
    case EventType::UpdateAvailable: return "UpdateAvailable";
    case EventType::UpdateStarted: return "UpdateStarted";
    case EventType::UpdateFailed: return "UpdateFailed";
    case EventType::UpdateCompleted: return "UpdateCompleted";
    case EventType::RollbackTriggered: return "RollbackTriggered";
    case EventType::CalibrationStarted: return "CalibrationStarted";
    case EventType::CalibrationCompleted: return "CalibrationCompleted";
    case EventType::CalibrationFailed: return "CalibrationFailed";
  }
  return "UnknownEvent";
}

EventBus::EventBus() {
  running_ = true;
  worker_ = std::thread([this] { workerLoop(); });
}

EventBus::~EventBus() { stop(); }

EventBus::Token EventBus::subscribe(EventType type, Handler handler) {
  std::lock_guard<std::mutex> lock(subs_mutex_);
  Token token = next_token_++;
  subs_.push_back({token, false, type, std::move(handler)});
  return token;
}

EventBus::Token EventBus::subscribeAll(Handler handler) {
  std::lock_guard<std::mutex> lock(subs_mutex_);
  Token token = next_token_++;
  subs_.push_back({token, true, EventType::StateChanged, std::move(handler)});
  return token;
}

void EventBus::unsubscribe(Token token) {
  std::lock_guard<std::mutex> lock(subs_mutex_);
  for (auto it = subs_.begin(); it != subs_.end(); ++it) {
    if (it->token == token) {
      subs_.erase(it);
      return;
    }
  }
}

void EventBus::publish(Event event) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!running_) return;
    queue_.push_back(std::move(event));
    ++in_flight_;
  }
  queue_cv_.notify_one();
}

void EventBus::dispatch(const Event& event) {
  // Copy matching handlers under lock, then invoke without the lock so a handler that
  // (un)subscribes or publishes cannot deadlock.
  std::vector<Handler> handlers;
  {
    std::lock_guard<std::mutex> lock(subs_mutex_);
    for (const auto& sub : subs_) {
      if (sub.all || sub.type == event.type) handlers.push_back(sub.handler);
    }
  }
  for (const auto& h : handlers) {
    // A misbehaving subscriber is isolated here, never propagated to the publisher or peers.
    // core stays dependency-free (no logging link), so failures go to stderr, which the journal
    // captures; the fault-isolation guarantee does not depend on the logger being up.
    try {
      h(event);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[eventbus] subscriber threw handling %s: %s\n", toString(event.type),
                   e.what());
    } catch (...) {
      std::fprintf(stderr, "[eventbus] subscriber threw (non-std) handling %s\n",
                   toString(event.type));
    }
  }
}

void EventBus::workerLoop() {
  for (;;) {
    Event event{EventType::StateChanged, ""};
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
      if (!running_ && queue_.empty()) return;
      event = std::move(queue_.front());
      queue_.pop_front();
    }
    dispatch(event);
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      --in_flight_;
      if (in_flight_ == 0) idle_cv_.notify_all();
    }
  }
}

void EventBus::drain() {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  idle_cv_.wait(lock, [this] { return in_flight_ == 0; });
}

void EventBus::stop() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!running_) return;
    running_ = false;
  }
  queue_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

}  // namespace nexus::core
