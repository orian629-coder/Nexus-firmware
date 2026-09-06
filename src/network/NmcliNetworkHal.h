#pragma once

#include "network/INetworkHal.h"

namespace nexus::network {

// Real network HAL backed by NetworkManager's `nmcli` CLI (Raspberry Pi OS). Built when
// NEXUS_STUB_HAL is off. Runs nmcli under the service user via the polkit rule installed by
// scripts/install.sh — no root required.
class NmcliNetworkHal : public INetworkHal {
 public:
  core::Result<std::vector<WifiNetwork>> scanWifi() override;
  using INetworkHal::connectWifi;  // inherit the ssid+psk convenience overload
  core::Status connectWifi(const WifiConnectParams& params) override;
  core::Status disconnect() override;
  core::Result<NetworkStatus> status() override;
};

}  // namespace nexus::network
