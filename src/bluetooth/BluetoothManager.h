#pragma once

#include <memory>
#include <string>

#include "bluetooth/IBluetoothHal.h"
#include "core/EventBus.h"
#include "core/IService.h"

namespace nexus::bluetooth {

// Makes the speaker act as an A2DP sink ("a Bluetooth speaker"): a phone can pair and stream
// audio directly, which WirePlumber routes to the I2S output. All adapter/OS work lives behind
// IBluetoothHal, so this module's logic and event wiring are fully testable off-target with
// StubBluetoothHal.
//
// This is an optional, non-critical service: if no adapter is present the module starts
// Degraded and the rest of the system is unaffected (per the IService contract).
//
// Ported from the pre-redesign speaker-app (bluetooth/BluetoothManager) — the bluetoothctl
// bridge is preserved, restructured behind a HAL to match the module conventions here.
class BluetoothManager : public core::IService {
 public:
  // Takes ownership of the HAL. If null, a HAL is selected at construction based on
  // NEXUS_STUB_HAL. `alias` is the name the speaker advertises over Bluetooth.
  explicit BluetoothManager(core::EventBus* bus, std::string alias = "Nexus Audio",
                            std::unique_ptr<IBluetoothHal> hal = nullptr);

  std::string name() const override { return "bluetooth"; }
  core::Status start() override;
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }
  core::Status healthCheck() override;

  // (Re)open a pairing window (e.g. driven by a button or the web UI). seconds == 0 → no timeout.
  core::Status openPairingWindow(int seconds = 0);

  bool isConnected() const { return connected_; }
  const std::string& connectedDeviceName() const { return device_name_; }

 private:
  // Poll the HAL for connection changes and publish AudioStarted/AudioStopped accordingly.
  void refreshConnection();

  core::EventBus* bus_;
  std::string alias_;
  std::unique_ptr<IBluetoothHal> hal_;
  core::ServiceState state_ = core::ServiceState::Stopped;
  bool connected_ = false;
  std::string device_name_;
};

}  // namespace nexus::bluetooth
