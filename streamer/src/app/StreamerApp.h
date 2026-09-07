#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/CommandGateway.h"
#include "app/LinkReporterService.h"
#include "audio/AudioEngine.h"
#include "config/StreamerConfigManager.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "core/Result.h"
#include "discovery/DiscoveryService.h"
#include "dsp/MasterDsp.h"
#include "group/SpeakerRegistry.h"
#include "group/ZoneManager.h"
#include "sources/SourceFactory.h"
#include "identity/StreamerIdentity.h"
#include "provisioning/AutoPairWorker.h"
#include "provisioning/ProvisioningWindow.h"
#include "send/IPacketSink.h"
#include "state/MonitorService.h"
#include "state/SpeakerStateStore.h"
#include "web/StreamerApiRouter.h"

namespace nexus::streamer::app {

// Top-level composition root for the streamer, mirroring the speaker's Application: build the
// services, start them in a fixed order, and stop them in reverse.
//
// The point of this class is that the control UI, the control channel, and the audio engine are now
// services in ONE process rather than mutually exclusive main() branches. Before Phase 1 you could
// either serve the UI or stream audio, never both, so the UI's play button could not produce sound.
//
// Startup order (reverse on shutdown):
//   1. audio-engine   — thread up and idle, so a play() can be served the moment the UI asks
//   2. link-reporter  — needs the gateway, harmless if no speakers are registered yet
//   3. web            — started last: once it is listening, requests can arrive immediately, so
//                       everything they touch must already be running
class StreamerApp {
 public:
  struct Options {
    int web_port = 8090;
    double lead_seconds = 0.18;
    int sample_rate = 48000;
    int channels = 2;
    std::uint32_t block_frames = 480;
    int link_interval_ms = 2000;
    bool enable_link_reporter = true;
    // How often every speaker is asked for its real state, and how many consecutive unreachable
    // polls mark it offline. 5 s × 3 ≈ 15 s to notice a speaker that lost power.
    int poll_interval_ms = 5000;
    int offline_threshold = 3;
    bool enable_monitor = true;
    // Where the persisted config lives. Empty disables persistence entirely (tests).
    std::string config_path;
    // Loopback by default; --bind 0.0.0.0 opts into LAN exposure.
    std::string bind_address = "127.0.0.1";
    // mDNS advertisement. Null on builds without Avahi (a dev Mac) — the streamer still runs.
    std::shared_ptr<discovery::IStreamerDiscovery> discovery;
  };

  // Dependencies are injected so the whole app is testable off-target: tests pass a
  // MemoryPacketSink, a loopback ILineTransport, and a StubWebTransport.
  StreamerApp(const identity::StreamerIdentity& id, control::ILineTransport& line,
              send::IPacketSink& sink, nexus::web::IWebTransport& web, Options options);
  StreamerApp(const identity::StreamerIdentity& id, control::ILineTransport& line,
              send::IPacketSink& sink, nexus::web::IWebTransport& web)
      : StreamerApp(id, line, sink, web, Options{}) {}
  ~StreamerApp();

  // Build + start every service in order. On any failure, already-started services are stopped.
  core::Status startup();

  // Stop every started service in reverse order. Idempotent.
  core::Status shutdown();

  // Block until requestShutdown() is called, then shut down. Returns the process exit code.
  int run();
  void requestShutdown() { running_ = false; }

  // ── accessors for tests and for main() ──
  audio::AudioEngine& audio() { return *audio_; }
  CommandGateway& gateway() { return *gateway_; }
  group::SpeakerRegistry& registry() { return registry_; }
  group::ZoneManager& zones() { return *zones_; }
  state::SpeakerStateStore& store() { return store_; }
  state::MonitorService& monitor() { return *monitor_; }
  core::EventBus& bus() { return bus_; }
  web::StreamerApiRouter& router() { return *router_; }
  streamer::dsp::MasterDsp& masterDsp() { return *master_dsp_; }
  // Zero-touch provisioning window (Task 4). Shared with the router (GET/POST
  // /api/provisioning-window) and, from Task 7, the onboarding worker.
  provisioning::ProvisioningWindow& provisioningWindow() { return provisioning_window_; }
  config::StreamerConfigManager* config() { return config_.get(); }
  const std::string& authToken() const { return auth_token_; }

  // Start a zone playing `spec`, resolving members to audio targets. This is the path the UI uses.
  core::Status playZone(const std::string& zone_id, const sources::SourceSpec& spec);

  // Point the CURRENT stream at a different set of speakers, without restarting the source.
  //
  // Distinct from playZone on purpose: constructing a source spawns a capture process (maccapture
  // on macOS), so calling play again to change the target set would restart the audio and be
  // audible as a gap. Retargeting only swaps the fan-out, which is what "tick another speaker
  // while music is playing" should do.
  core::Status retargetZone(const std::string& zone_id);

  // Set the audio fan-out targets and begin streaming `source`. Used by --stream at boot and by the
  // UI's play button via the router's TransportControl seam.
  core::Status playTo(const std::vector<audio::Endpoint>& targets,
                      std::unique_ptr<sources::IAudioSource> source);

 private:
  // Resolves a transport action from the UI into engine calls. Wired into the router.
  core::Status onTransport(const std::string& action, const group::Speaker& target);

  const identity::StreamerIdentity& id_;
  control::ILineTransport& line_;
  send::IPacketSink& sink_;
  nexus::web::IWebTransport& web_;
  Options options_;

  core::EventBus bus_;
  group::SpeakerRegistry registry_;
  state::SpeakerStateStore store_{&bus_};
  // Stable for the app's lifetime: the router holds a raw pointer to it, and Task 7's onboarding
  // worker will too.
  provisioning::ProvisioningWindow provisioning_window_;

  std::unique_ptr<config::StreamerConfigManager> config_;
  std::unique_ptr<group::ZoneManager> zones_;
  std::unique_ptr<CommandGateway> gateway_;
  // Master chain. Declared BEFORE audio_ so it is destroyed AFTER it: members are destroyed in
  // reverse declaration order, and the audio thread (owned by audio_, joined in its destructor)
  // calls into this object through the DSP hook. Destroying it first would leave the still-running
  // thread calling into freed memory.
  std::unique_ptr<streamer::dsp::MasterDsp> master_dsp_;
  std::unique_ptr<audio::AudioEngine> audio_;
  std::unique_ptr<LinkReporterService> link_;
  std::unique_ptr<state::MonitorService> monitor_;
  std::unique_ptr<discovery::DiscoveryService> discovery_;
  std::unique_ptr<nexus::web::Authentication> auth_;
  std::unique_ptr<web::StreamerApiRouter> router_;
  std::string auth_token_;

  // Zero-touch provisioning (Task 7): a dedicated discovery handle for browsing `_nexus-speaker._tcp`
  // setup beacons — separate from `options_.discovery` (which is only used to ADVERTISE this
  // streamer and may be null on a dev host with no Avahi wired up). This one is always constructed,
  // so the sweep thread has something to call even when the window stays closed forever.
  // Declared BEFORE autopair_worker_ so it outlives the worker on teardown (members are destroyed in
  // reverse declaration order): the worker holds a reference to it for its whole life.
  std::unique_ptr<discovery::IStreamerDiscovery> autopair_discovery_;
  std::unique_ptr<provisioning::AutoPairWorker> autopair_worker_;
  // Background sweep: wakes every ~3 s (or immediately on shutdown, via the condition variable) and
  // calls sweepOnce(), which is a no-op the instant the window is closed. Joined in shutdown() before
  // any member it touches (registry_, config_, line_, id_) is torn down.
  std::thread autopair_thread_;
  std::mutex autopair_mutex_;
  std::condition_variable autopair_cv_;
  std::atomic<bool> autopair_stop_{false};

  // Persist the current registry + zones back to disk. Called only on user-initiated changes —
  // never on state/online updates, which are intentionally ephemeral.
  void persist();

  // Services in start order; shutdown walks this in reverse. Non-owning.
  std::vector<core::IService*> ordered_;
  std::vector<core::IService*> started_;
  bool web_started_ = false;

  std::atomic<bool> running_{false};
};

}  // namespace nexus::streamer::app
