#include "pairing/PairingService.h"

#include <nlohmann/json.hpp>

#include "logging/Logger.h"
#include "pairing/PairingValidator.h"

namespace nexus::pairing {

using core::ErrorCode;
using core::Result;
using core::SecretString;
using core::ServiceState;
using core::Status;

PairingService::PairingService(core::EventBus* bus, identity::KeyManager* keys,
                               config::ConfigManager* config, storage::SecureStorage* secrets)
    : bus_(bus), keys_(keys), config_(config), secrets_(secrets) {}

Status PairingService::start() {
  state_ = ServiceState::Running;
  return Status::success();
}

Status PairingService::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  setup_mode_ = false;
  setup_code_.clear();
  state_ = ServiceState::Stopped;
  return Status::success();
}

bool PairingService::inSetupMode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return setup_mode_;
}

bool PairingService::isPaired() const { return config_ && config_->get().pairing.paired; }

Status PairingService::beginSetupMode(const std::string& setup_code, std::int64_t now_epoch,
                                      int ttl_seconds) {
  if (setup_code.empty()) return Status::error(ErrorCode::InvalidArg, "empty setup code");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    setup_mode_ = true;
    setup_code_ = setup_code;
    code_expiry_epoch_ = now_epoch + ttl_seconds;
  }
  NX_LOG_INFO("pairing", "entered setup mode (code hidden), ttl=" + std::to_string(ttl_seconds));
  if (bus_) bus_->publish(core::Event{core::EventType::PairingStarted, "pairing"});
  return Status::success();
}

Result<PairingResult> PairingService::processRequest(const PairingRequest& req,
                                                     std::int64_t now_epoch) {
  std::string code;
  std::int64_t expiry;
  bool in_setup;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    code = setup_code_;
    expiry = code_expiry_epoch_;
    in_setup = setup_mode_;
  }

  auto fail = [&](Status s) -> Result<PairingResult> {
    NX_LOG_ERROR("pairing", s.code(), "pairing failed: " + s.message());
    if (bus_) bus_->publish(core::Event{core::EventType::PairingFailed, "pairing"});
    return s;
  };

  if (!in_setup) return fail(Status::error(ErrorCode::PermissionDenied, "not in setup mode"));

  Status v = PairingValidator::validate(req, code, now_epoch, expiry);
  if (!v.ok()) return fail(v);

  // Decrypt the sealed Wi-Fi credentials with the device X25519 secret key.
  auto opened = keys_->openSealed(req.sealed_wifi_b64);
  if (!opened.ok()) return fail(Status::error(ErrorCode::CryptoError, "cannot open sealed wifi"));

  WifiCredentials wifi;
  try {
    auto j = nlohmann::json::parse(opened.value().begin(), opened.value().end());
    wifi.ssid = j.at("ssid").get<std::string>();
    wifi.psk = j.at("psk").get<std::string>();
  } catch (const std::exception& e) {
    return fail(Status::error(ErrorCode::Corrupt, std::string("bad wifi payload: ") + e.what()));
  }

  PairingResult result;
  result.streamer_id = req.streamer_id;
  result.streamer_public_key = req.streamer_public_key;
  result.site_id = req.site_id;
  result.speaker_name = req.initial_speaker_name;
  result.wifi = wifi;

  Status p = persistPairing(result);
  if (!p.ok()) return fail(p);

  // Pairing is one-time: leave setup mode so a second request can't re-pair.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    setup_mode_ = false;
    setup_code_.clear();
  }
  NX_LOG_INFO("pairing", "paired with streamer " + result.streamer_id);
  if (bus_) {
    bus_->publish(core::Event{core::EventType::PairingCompleted, "pairing",
                              {{"streamer_id", result.streamer_id}}});
  }
  return result;
}

Status PairingService::persistPairing(const PairingResult& result) {
  // Secrets (Wi-Fi PSK/SSID) go to SecureStorage only — never config, never logs.
  if (secrets_) {
    Status s1 = secrets_->put(kWifiSsidSecret, SecretString(result.wifi.ssid));
    Status s2 = secrets_->put(kWifiPskSecret, SecretString(result.wifi.psk));
    if (!s1.ok()) return s1;
    if (!s2.ok()) return s2;
  }
  // Config records only public pairing info + the flag.
  if (config_) {
    return config_->update([&](config::SpeakerConfig& c) {
      c.pairing.paired = true;
      c.pairing.streamer_id = result.streamer_id;
      c.pairing.streamer_public_key = result.streamer_public_key;
      c.network.wifi_configured = true;
    });
  }
  return Status::success();
}

}  // namespace nexus::pairing
