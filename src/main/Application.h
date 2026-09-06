#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "amplifier/AmplifierManager.h"
#include "audio/AudioBuffer.h"
#include "audio/AudioReceiver.h"
#include "audio/PlaybackEngine.h"
#include "audio/StreamSync.h"
#include "pairing/ProvisioningController.h"
#include "calibration/CalibrationManager.h"
#include "config/ConfigManager.h"
#include "diagnostics/DiagnosticService.h"
#include "control/CommandServer.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "discovery/DiscoveryService.h"
#include "dsp/DspEngine.h"
#include "identity/DeviceIdentity.h"
#include "identity/KeyManager.h"
#include "microphone/MicrophoneManager.h"
#include "network/NetworkManager.h"
#include "pairing/PairingService.h"
#include "status/StatusService.h"
#include "storage/SecureStorage.h"
#include "system/SystemManager.h"
#include "system/Watchdog.h"
#include "updater/UpdateManager.h"
#include "web/WebServer.h"

namespace nexus {

// Options resolved from the command line / environment.
struct AppOptions {
  std::string config_path = "/etc/nexus-speaker/config.json";
  std::string identity_path = "/etc/nexus-speaker/identity/factory.json";
  std::string secure_dir = "/var/lib/nexus-speaker/secure";
  std::string calibration_dir = "/var/lib/nexus-speaker/calibration";
  std::string log_path = "/var/log/nexus-speaker/speaker.log";
  std::string binary_path = "/usr/local/bin/nexus-speaker";  // OTA install target
  std::string vendor_public_key_b64;                          // update signing key (empty = OTA off)
};

// Owns the event bus and every service. Brings services up in the specification's startup order
// and tears them down in reverse. Critical services (logging, config, identity) that fail to
// start abort the boot; non-critical services degrade and the process keeps running.
class Application {
 public:
  explicit Application(AppOptions opts);
  ~Application();

  // Initialize logging, construct + start all services. Returns non-ok if a critical service
  // failed. Does not block.
  core::Status startup();

  // Block until requestShutdown() is called (e.g. by a signal handler), then stop services.
  int run();

  // Signal-safe shutdown request.
  void requestShutdown();

  // Test hook: stop all services in reverse order without blocking on run().
  void shutdown();

  system::SystemManager& system() { return *system_; }
  config::ConfigManager& config() { return *config_; }
  identity::DeviceIdentity& identity() { return *identity_; }
  network::NetworkManager& network() { return *network_; }
  discovery::DiscoveryService& discovery() { return *discovery_; }
  pairing::PairingService& pairing() { return *pairing_; }
  control::CommandServer& control() { return *control_; }
  status::StatusService& status() { return *status_; }
  audio::AudioReceiver& audio() { return *audio_; }
  audio::PlaybackEngine& playback() { return *playback_; }
  dsp::DspEngine& dsp() { return *dsp_; }
  amplifier::AmplifierManager& amplifier() { return *amplifier_; }
  microphone::MicrophoneManager& microphone() { return *microphone_; }
  diagnostics::DiagnosticService& diagnostics() { return *diagnostics_; }
  calibration::CalibrationManager& calibration() { return *calibration_; }
  web::WebServer& web() { return *web_; }
  updater::UpdateManager& updater() { return *updater_; }
  system::Watchdog& watchdog() { return *watchdog_; }

 private:
  void buildServices();
  core::Status startInOrder();
  // Emit a short tone so an installer can tell WHICH physical speaker a row in the controller's
  // list is. These units have no addressable indicator LED, so sound is the identification channel.
  std::pair<bool, std::string> playIdentifyTone();

  AppOptions opts_;
  core::EventBus bus_;

  std::unique_ptr<storage::SecureStorage> secure_;
  std::unique_ptr<identity::KeyManager> keys_;
  std::unique_ptr<config::ConfigManager> config_;
  std::unique_ptr<identity::DeviceIdentity> identity_;
  std::unique_ptr<system::SystemManager> system_;
  std::unique_ptr<network::NetworkManager> network_;
  std::unique_ptr<discovery::DiscoveryService> discovery_;
  std::unique_ptr<pairing::PairingService> pairing_;
  std::unique_ptr<pairing::ProvisioningController> provisioning_;
  std::unique_ptr<control::CommandServer> control_;
  std::unique_ptr<status::StatusService> status_;
  audio::AudioBuffer audio_buffer_;
  audio::StreamSync audio_sync_;
  std::unique_ptr<audio::AudioReceiver> audio_;
  std::unique_ptr<audio::PlaybackEngine> playback_;
  std::unique_ptr<dsp::DspEngine> dsp_;
  std::unique_ptr<amplifier::AmplifierManager> amplifier_;
  std::unique_ptr<microphone::MicrophoneManager> microphone_;
  std::unique_ptr<diagnostics::DiagnosticService> diagnostics_;
  std::shared_ptr<calibration::CalibrationProfiles> cal_profiles_;
  std::unique_ptr<calibration::CalibrationManager> calibration_;
  std::unique_ptr<web::WebServer> web_;
  std::unique_ptr<updater::UpdateManager> updater_;
  std::unique_ptr<system::Watchdog> watchdog_;

  web::ApiContext buildApiContext();

  // Ordered list of every IService, in startup order. Owned via the unique_ptrs above and the
  // stub owners vector below.
  std::vector<std::unique_ptr<core::IService>> owned_stubs_;
  std::vector<core::IService*> ordered_;   // startup order
  std::vector<core::IService*> started_;   // successfully started, for reverse shutdown

  std::atomic<bool> shutdown_requested_{false};
  bool stopped_ = false;
};

}  // namespace nexus
