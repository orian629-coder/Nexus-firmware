#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::bluetooth {

// Current Bluetooth link state, as reported by the HAL.
struct BluetoothStatus {
  bool available = false;    // an adapter is present and usable
  bool discoverable = false;  // advertising for pairing
  bool connected = false;     // a phone/source is connected
  std::string device_name;    // name of the connected device (empty if none)
};

// Hardware/OS abstraction for turning the speaker into an A2DP sink ("a Bluetooth speaker").
// On the Pi this is backed by `bluetoothctl` (see BluetoothctlHal); on dev hosts a stub
// simulates an adapter so BluetoothManager's logic and event wiring are fully testable
// off-target. Selected at build time by NEXUS_STUB_HAL.
//
// Audio routing itself is handled by PipeWire + WirePlumber: once a source is paired and
// playing, WirePlumber routes it to the I2S sink automatically. The HAL only manages
// adapter power, discoverability, the alias, and the auto-accept pairing agent.
class IBluetoothHal {
 public:
  virtual ~IBluetoothHal() = default;

  // Bring the adapter up as an A2DP sink with the given alias: power on, set alias, become
  // pairable + discoverable, and start a persistent agent that auto-accepts pairing.
  virtual core::Status enableSink(const std::string& alias) = 0;

  // (Re)open a pairing window. seconds == 0 means no timeout (always discoverable).
  virtual core::Status openPairingWindow(int seconds) = 0;

  // Stop advertising and tear down the pairing agent.
  virtual core::Status disable() = 0;

  virtual core::Result<BluetoothStatus> status() = 0;
};

// Simulated Bluetooth adapter for development hosts. Reports an available adapter; enableSink()
// marks it discoverable so higher layers can be exercised without real hardware.
class StubBluetoothHal : public IBluetoothHal {
 public:
  core::Status enableSink(const std::string& alias) override {
    status_.available = true;
    status_.discoverable = true;
    alias_ = alias;
    return core::Status::success();
  }
  core::Status openPairingWindow(int) override {
    status_.discoverable = true;
    return core::Status::success();
  }
  core::Status disable() override {
    status_.discoverable = false;
    return core::Status::success();
  }
  core::Result<BluetoothStatus> status() override { return status_; }

  // Test seam: simulate a source connecting/disconnecting.
  void simulateConnect(const std::string& name) {
    status_.connected = true;
    status_.device_name = name;
  }
  void simulateDisconnect() {
    status_.connected = false;
    status_.device_name.clear();
  }

 private:
  BluetoothStatus status_{true, false, false, ""};
  std::string alias_;
};

}  // namespace nexus::bluetooth
