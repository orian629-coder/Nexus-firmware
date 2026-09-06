#include "network/NmcliNetworkHal.h"

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>

namespace nexus::network {

using core::ErrorCode;
using core::Result;
using core::Status;

namespace {

// Run a command, capture stdout. Returns exit status via `rc`. Arguments must be pre-escaped by
// the caller (we only pass fixed subcommands + validated SSIDs quoted below).
Result<std::string> run(const std::string& cmd, int& rc) {
  std::array<char, 256> buf{};
  std::string out;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) return Status::error(ErrorCode::IoError, "popen failed");
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
  rc = ::pclose(pipe);
  return out;
}

// Minimal shell-quote for a single argument (SSID/PSK). Wrap in single quotes and escape any
// embedded single quotes.
std::string shq(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

}  // namespace

Result<std::vector<WifiNetwork>> NmcliNetworkHal::scanWifi() {
  int rc = 0;
  auto out = run("nmcli -t -f SSID,SIGNAL device wifi list 2>/dev/null", rc);
  if (!out.ok()) return out.status();
  std::vector<WifiNetwork> nets;
  std::istringstream iss(out.value());
  std::string line;
  while (std::getline(iss, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    WifiNetwork n;
    n.ssid = line.substr(0, colon);
    try {
      n.signal_dbm = std::stoi(line.substr(colon + 1));  // nmcli SIGNAL is 0-100, not dBm
    } catch (...) {
      n.signal_dbm = 0;
    }
    if (!n.ssid.empty()) nets.push_back(n);
  }
  return nets;
}

Status NmcliNetworkHal::connectWifi(const WifiConnectParams& p) {
  int rc = 0;
  if (p.ssid.empty()) return Status::error(ErrorCode::InvalidArg, "ssid required");

  // wlan0 hosts the setup hotspot (AP mode) during onboarding. A single Wi-Fi radio cannot be an AP
  // and a client at the same time, so the AP MUST come down before we associate as a client — else
  // the connect competes with the AP and tears it down mid-handshake, which the user sees as
  // "connection failed" (and the phone drops off the hotspot). So: tear the AP down, then connect.
  // The caller (setup UI) warns the user the setup network will disappear during this step.
  run("nmcli connection down nexus-setup-ap >/dev/null 2>&1", rc);
  run("nmcli connection delete nexus-setup-ap >/dev/null 2>&1", rc);

  const bool advanced = p.hidden || !p.band.empty() || !p.static_ip.empty();
  if (!advanced) {
    // Simple path: the one-shot connect command handles the common broadcast-SSID + DHCP case and
    // creates/activates a profile in one step.
    std::string cmd = "nmcli device wifi connect " + shq(p.ssid) + " password " + shq(p.psk) +
                      " ifname wlan0 >/dev/null 2>&1";
    run(cmd, rc);
    if (rc != 0) return Status::error(ErrorCode::IoError, "nmcli connect failed");
    return Status::success();
  }

  // Advanced path: build a named connection profile so we can set hidden SSID, band, and a static
  // IPv4 — knobs the one-shot `device wifi connect` doesn't expose. Recreate it each time so a
  // retry with different settings doesn't stack duplicate profiles.
  const std::string con = "nexus-wifi";
  run("nmcli connection delete " + con + " >/dev/null 2>&1", rc);

  std::string add = "nmcli connection add type wifi ifname wlan0 con-name " + shq(con) +
                    " ssid " + shq(p.ssid);
  if (p.hidden) add += " 802-11-wireless.hidden yes";
  if (!p.band.empty()) add += " 802-11-wireless.band " + shq(p.band);  // "bg" (2.4) | "a" (5)
  add += " >/dev/null 2>&1";
  run(add, rc);
  if (rc != 0) return Status::error(ErrorCode::IoError, "nmcli profile add failed");

  // Security: WPA-PSK when a password is provided (open network otherwise).
  // Each step's rc must be checked HERE: `rc` is reused by every run() call, so letting a failure
  // fall through means the final `connection up` overwrites it and a rejected PSK (or a malformed
  // static IP) is reported as a successful connection.
  if (!p.psk.empty()) {
    run("nmcli connection modify " + shq(con) +
            " 802-11-wireless-security.key-mgmt wpa-psk "
            "802-11-wireless-security.psk " +
            shq(p.psk) + " >/dev/null 2>&1",
        rc);
    if (rc != 0) {
      run("nmcli connection delete " + shq(con) + " >/dev/null 2>&1", rc);
      return Status::error(ErrorCode::InvalidArg, "wifi password rejected");
    }
  }

  // Static IPv4 vs DHCP.
  if (!p.static_ip.empty()) {
    std::string ipcmd = "nmcli connection modify " + shq(con) + " ipv4.method manual ipv4.addresses " +
                        shq(p.static_ip);
    if (!p.gateway.empty()) ipcmd += " ipv4.gateway " + shq(p.gateway);
    if (!p.dns.empty()) ipcmd += " ipv4.dns " + shq(p.dns);
    ipcmd += " >/dev/null 2>&1";
    run(ipcmd, rc);
    if (rc != 0) {
      run("nmcli connection delete " + shq(con) + " >/dev/null 2>&1", rc);
      return Status::error(ErrorCode::InvalidArg, "invalid static IP settings");
    }
  }

  run("nmcli connection up " + shq(con) + " >/dev/null 2>&1", rc);
  if (rc != 0) return Status::error(ErrorCode::IoError, "nmcli connect failed");
  return Status::success();
}

Status NmcliNetworkHal::disconnect() {
  int rc = 0;
  run("nmcli device disconnect wlan0 >/dev/null 2>&1", rc);
  return Status::success();
}

Result<NetworkStatus> NmcliNetworkHal::status() {
  int rc = 0;
  NetworkStatus st;
  // Determine REAL uplink connectivity. The setup hotspot puts wlan0 into AP mode with a static
  // 10.42.0.x address; that is NOT connectivity and must be ignored, or the speaker (and the
  // ProvisioningController that keys off it) would think it's online while only hosting the setup
  // AP. So we look for a connected device whose connection is a genuine uplink: any ethernet, or a
  // Wi-Fi device acting as a client (connection profile != the hotspot AP). p2p-dev-wlan0 and the
  // "nexus-setup-ap" connection are excluded.
  //
  // Fields: DEVICE:TYPE:STATE:CONNECTION per line.
  auto devs = run("nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device 2>/dev/null", rc);
  std::string uplink_device, uplink_type;
  if (devs.ok()) {
    std::stringstream ss(devs.value());
    std::string line;
    while (std::getline(ss, line)) {
      // Split into 4 colon fields (CONNECTION may itself contain colons; take the first 3 splits).
      auto p1 = line.find(':');
      if (p1 == std::string::npos) continue;
      auto p2 = line.find(':', p1 + 1);
      if (p2 == std::string::npos) continue;
      auto p3 = line.find(':', p2 + 1);
      if (p3 == std::string::npos) continue;
      const std::string device = line.substr(0, p1);
      const std::string type = line.substr(p1 + 1, p2 - p1 - 1);
      const std::string state = line.substr(p2 + 1, p3 - p2 - 1);
      const std::string conn = line.substr(p3 + 1);
      if (state != "connected") continue;
      if (type == "wifi-p2p" || device.rfind("p2p-", 0) == 0) continue;  // p2p pseudo-device
      if (conn == "nexus-setup-ap") continue;                            // the hotspot AP itself
      if (type == "ethernet" || type == "wifi") {
        uplink_device = device;
        uplink_type = type;
        if (type == "ethernet") break;  // prefer wired
      }
    }
  }

  if (!uplink_device.empty()) {
    auto ip = run("nmcli -t -f IP4.ADDRESS device show " + uplink_device + " 2>/dev/null | head -1",
                  rc);
    if (ip.ok()) {
      std::string v = ip.value();
      auto eq = v.find(':');
      if (eq != std::string::npos) {
        st.ip_address = v.substr(eq + 1);
        auto slash = st.ip_address.find('/');
        if (slash != std::string::npos) st.ip_address = st.ip_address.substr(0, slash);
        while (!st.ip_address.empty() &&
               (st.ip_address.back() == '\n' || st.ip_address.back() == '\r'))
          st.ip_address.pop_back();
      }
    }
  }
  st.connected = !st.ip_address.empty();
  st.mode = st.connected ? (uplink_type == "ethernet" ? "ethernet" : "wifi") : "";
  return st;
}

}  // namespace nexus::network
