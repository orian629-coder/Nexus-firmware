#include "discovery/ApJoiner.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <sstream>
#include <thread>

namespace nexus::streamer::discovery {

using core::ErrorCode;
using core::Status;

namespace {

// pclose() returns a wait status, not an exit code (an exit of 1 comes back as 256), so the exit
// code is extracted with WEXITSTATUS rather than compared raw.
std::string run(const std::string& cmd, int& rc) {
  std::array<char, 512> buf{};
  std::string out;
  rc = -1;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) return out;
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
  const int status = ::pclose(pipe);
  rc = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
  return out;
}

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

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

}  // namespace

Status ApJoiner::join(const std::string& ssid, int timeout_s) {
  if (ssid.empty()) return Status::error(ErrorCode::InvalidArg, "ssid required");
  leave();  // never stack profiles from a previous, half-finished attempt

  int rc = 0;
  // A named profile (rather than the one-shot `device wifi connect`) is what makes the join
  // reversible: leave() deletes exactly this profile and nothing else, so the radio's previous
  // configuration is untouched.
  const std::string profile = "nexus-onboard";
  run("nmcli connection delete " + shq(profile) + " >/dev/null 2>&1", rc);

  std::string add = "nmcli connection add type wifi ifname " + shq(iface_) + " con-name " +
                    shq(profile) + " ssid " + shq(ssid) +
                    " connection.autoconnect no >/dev/null 2>&1";
  run(add, rc);
  if (rc != 0) return Status::error(ErrorCode::IoError, "cannot create onboarding profile");

  // The setup AP is open; no security settings are applied deliberately.
  const std::string up = "nmcli -w " + std::to_string(timeout_s) + " connection up " +
                         shq(profile) + " >/dev/null 2>&1";
  run(up, rc);
  if (rc != 0) {
    run("nmcli connection delete " + shq(profile) + " >/dev/null 2>&1", rc);
    return Status::error(ErrorCode::IoError, "cannot join setup network '" + ssid + "'");
  }

  profile_ = profile;
  return Status::success();
}

Status ApJoiner::leave() {
  if (profile_.empty()) {
    // Still clear any profile stranded by a crash mid-join, so the next attempt starts clean.
    int rc = 0;
    run("nmcli connection delete 'nexus-onboard' >/dev/null 2>&1", rc);
    return Status::success();
  }
  int rc = 0;
  run("nmcli connection down " + shq(profile_) + " >/dev/null 2>&1", rc);
  run("nmcli connection delete " + shq(profile_) + " >/dev/null 2>&1", rc);
  profile_.clear();
  return Status::success();
}

std::string ApJoiner::gateway() const {
  if (profile_.empty()) return {};
  int rc = 0;
  const std::string out =
      run("nmcli -g IP4.GATEWAY device show " + shq(iface_) + " 2>/dev/null", rc);
  if (rc != 0) return {};
  std::istringstream ss(out);
  std::string line;
  while (std::getline(ss, line)) {
    line = trim(line);
    if (!line.empty() && line != "--") return line;
  }
  return {};
}

}  // namespace nexus::streamer::discovery
