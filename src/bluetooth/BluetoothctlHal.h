#pragma once

#include <sys/types.h>

#include <string>

#include "bluetooth/IBluetoothHal.h"

namespace nexus::bluetooth {

// Real Bluetooth HAL backed by the `bluetoothctl` CLI (Raspberry Pi OS, BlueZ). Built when
// NEXUS_STUB_HAL is off. Wraps bluetoothctl as a subprocess, mirroring how NmcliNetworkHal
// wraps nmcli — no D-Bus/library dependency.
//
// Auto-accepting pairing (Just Works) requires a *persistent* bluetoothctl session: a one-shot
// invocation does not keep an agent registered. So enableSink() forks a long-lived bluetoothctl
// process with `agent NoInputNoOutput` / `default-agent` and keeps its stdin open; disable()
// closes stdin (EOF) and reaps it.
class BluetoothctlHal : public IBluetoothHal {
 public:
  ~BluetoothctlHal() override;

  core::Status enableSink(const std::string& alias) override;
  core::Status openPairingWindow(int seconds) override;
  core::Status disable() override;
  core::Result<BluetoothStatus> status() override;

 private:
  // Run one one-shot `bluetoothctl <args>` and return {exitCode, stdout+stderr}.
  struct CmdResult {
    int code;
    std::string out;
  };
  static CmdResult btctl(const std::string& args);

  void startAgent();  // fork the persistent auto-accept agent
  void stopAgent();   // close its stdin + kill/reap it

  pid_t agent_pid_ = -1;    // persistent agent process (-1 = not running)
  int agent_stdin_ = -1;    // write-end of its stdin, held open to keep the session alive
};

}  // namespace nexus::bluetooth
