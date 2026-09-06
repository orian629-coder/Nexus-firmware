#include "discovery/ApScanner.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <sstream>

namespace nexus::streamer::discovery {

namespace {

// Run a command and capture stdout. `rc` receives the command's EXIT CODE.
//
// pclose() returns a wait status, not an exit code — the exit code lives in the high byte, so a
// command that exited 1 yields 256. Comparing that raw value against small numbers silently
// misreads every result, which is why it goes through WEXITSTATUS here.
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

// Split an nmcli terse line on unescaped ':'. nmcli escapes a literal colon as "\:", which matters
// here because every BSSID is full of them (AA\:BB\:CC\:...).
std::vector<std::string> splitTerse(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) {
      cur += line[i + 1];  // unescape: "\:" -> ":"
      ++i;
      continue;
    }
    if (line[i] == ':') {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += line[i];
  }
  out.push_back(cur);
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

}  // namespace

std::vector<ScannedAp> scanAccessPoints(const std::string& iface, const std::string& ssid_prefix,
                                        bool rescan) {
  std::vector<ScannedAp> found;
  int rc = 0;

  // --rescan yes forces a fresh sweep instead of returning NetworkManager's cache, which can be
  // minutes stale — long enough that a speaker just powered on would not appear.
  std::string cmd = "nmcli -t -f SSID,BSSID,SIGNAL,SECURITY device wifi list ifname " + shq(iface);
  if (rescan) cmd += " --rescan yes";
  cmd += " 2>/dev/null";

  const std::string out = run(cmd, rc);
  if (rc != 0) return found;  // no Wi-Fi radio, interface busy, or NetworkManager unavailable

  std::istringstream ss(out);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    const auto f = splitTerse(line);
    if (f.size() < 4) continue;
    ScannedAp ap;
    ap.ssid = f[0];
    if (ap.ssid.empty()) continue;  // hidden AP — cannot be a speaker we are meant to recognise
    if (ap.ssid.rfind(ssid_prefix, 0) != 0) continue;
    ap.bssid = f[1];
    try {
      ap.signal = std::stoi(f[2]);
    } catch (...) {
      ap.signal = 0;
    }
    // nmcli prints an empty SECURITY field for an open network.
    ap.secured = !f[3].empty();
    found.push_back(ap);
  }
  return found;
}

std::string macForIp(const std::string& ip) {
  if (ip.empty()) return {};
  // Reject anything that is not a bare IPv4 literal before it reaches a shell: this string comes
  // from the speaker registry, which is fed by discovery and by operator input.
  int dots = 0;
  for (char c : ip) {
    if (c == '.') {
      ++dots;
    } else if (c < '0' || c > '9') {
      return {};
    }
  }
  if (dots != 3) return {};

  int rc = 0;
  // `ip neigh` on Linux; `arp -n` elsewhere (macOS dev hosts). Both print the MAC as the token
  // after "lladdr"/"at", so one parse covers the two.
  std::string out = run("ip neigh show " + ip + " 2>/dev/null", rc);
  if (rc != 0 || out.empty()) out = run("arp -n " + ip + " 2>/dev/null", rc);
  if (out.empty()) return {};

  std::istringstream ss(out);
  std::string tok, prev;
  while (ss >> tok) {
    if ((prev == "lladdr" || prev == "at") && tok.find(':') != std::string::npos) return tok;
    prev = tok;
  }
  return {};
}

}  // namespace nexus::streamer::discovery
