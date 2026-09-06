#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "config/ConfigManager.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "identity/KeyManager.h"
#include "pairing/PairingTypes.h"
#include "storage/SecureStorage.h"

namespace nexus::pairing {

// Owns the secure pairing flow between this speaker and a Streamer.
//
// Flow:
//   1. beginSetupMode() generates a time-limited setup code (shown to the user, e.g. on screen /
//      QR) and marks the speaker pairable.
//   2. processRequest() validates an inbound PairingRequest (setup code + Ed25519 signature),
//      opens the sealed Wi-Fi box with the device X25519 key, persists the streamer identity to
//      config and the Wi-Fi PSK to SecureStorage, and completes pairing (one-time).
//   3. Emits PairingStarted / PairingCompleted / PairingFailed.
//
// Secrets (Wi-Fi PSK) go only to SecureStorage; config stores just the public streamer key and a
// paired flag.
class PairingService : public core::IService {
 public:
  static constexpr const char* kWifiPskSecret = "wifi_psk";
  static constexpr const char* kWifiSsidSecret = "wifi_ssid";

  PairingService(core::EventBus* bus, identity::KeyManager* keys, config::ConfigManager* config,
                 storage::SecureStorage* secrets);

  std::string name() const override { return "pairing"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Enter setup mode with a caller-supplied code (so tests are deterministic and the UI can show
  // it). `now_epoch` and `ttl_seconds` bound its validity. Publishes PairingStarted.
  core::Status beginSetupMode(const std::string& setup_code, std::int64_t now_epoch,
                              int ttl_seconds = 300);
  bool inSetupMode() const;
  const std::string& setupCode() const { return setup_code_; }

  // Validate + apply a pairing request at time `now_epoch`. On success, persists pairing and
  // returns the credentials the caller (NetworkManager) uses to join Wi-Fi. Publishes
  // PairingCompleted or PairingFailed.
  core::Result<PairingResult> processRequest(const PairingRequest& req, std::int64_t now_epoch);

  bool isPaired() const;

 private:
  core::Status persistPairing(const PairingResult& result);

  core::EventBus* bus_;
  identity::KeyManager* keys_;
  config::ConfigManager* config_;
  storage::SecureStorage* secrets_;

  mutable std::mutex mutex_;
  bool setup_mode_ = false;
  std::string setup_code_;
  std::int64_t code_expiry_epoch_ = 0;
  core::ServiceState state_ = core::ServiceState::Stopped;
};

}  // namespace nexus::pairing
