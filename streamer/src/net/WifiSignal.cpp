#include "net/WifiSignal.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace nexus::streamer::net {

namespace {

#ifdef __APPLE__
// macOS has no /proc, so the Linux reader below finds nothing and a Mac streamer silently reports
// no signal at all — which is why the speaker's kiosk showed an empty signal meter while streaming
// from a Mac. system_profiler exposes the associated network's RSSI without needing sudo (unlike
// wdutil), under:
//
//   Current Network Information:
//     <ssid>:
//       Signal / Noise: -51 dBm / -80 dBm
//
// Only the FIRST such block is the real Wi-Fi interface; later ones belong to awdl0 (AirDrop) and
// must be ignored. The call takes a moment, so the caller (LinkReporter, every ~2 s) is the right
// cadence for it — do not put this on a hot path.
std::optional<int> readRssiMac() {
  std::FILE* p = ::popen("system_profiler SPAirPortDataType 2>/dev/null", "r");
  if (!p) return std::nullopt;

  char line[512];
  bool in_current = false;
  std::optional<int> result;
  while (std::fgets(line, sizeof(line), p)) {
    const std::string s(line);
    if (s.find("Current Network Information:") != std::string::npos) {
      in_current = true;
      continue;
    }
    if (!in_current) continue;

    const auto sig = s.find("Signal / Noise:");
    if (sig == std::string::npos) continue;

    // "Signal / Noise: -51 dBm / -80 dBm" → take the first signed number.
    const auto minus = s.find('-', sig);
    if (minus != std::string::npos) {
      try {
        const int dbm = std::stoi(s.substr(minus));
        if (dbm < 0 && dbm > -120) result = dbm;  // plausible RSSI
      } catch (...) {
      }
    }
    break;  // first block only — later ones are awdl0, not the real link
  }
  ::pclose(p);
  return result;
}
#endif

}  // namespace

std::optional<int> WifiSignal::readRssiDbm() {
#ifdef __APPLE__
  return readRssiMac();
#endif
  // /proc/net/wireless format (two header lines, then one line per wireless interface):
  //   Inter-| sta-|   Quality        |   Discarded packets ...
  //    face | tus | link level noise | ...
  //   wlan0: 0000   58.  -52.  -256  ...
  // We want the "level" column (3rd numeric field), which is the RSSI in dBm. The values carry a
  // trailing '.' (fixed-point display artifact) that std::stoi ignores. We take the first interface
  // whose level parses to a plausible dBm (< 0), so a wired host with no wlanN line yields nullopt.
  std::FILE* f = std::fopen("/proc/net/wireless", "r");
  if (!f) return std::nullopt;

  char line[256];
  // Skip the two header lines.
  for (int i = 0; i < 2; ++i) {
    if (!std::fgets(line, sizeof(line), f)) {
      std::fclose(f);
      return std::nullopt;
    }
  }

  std::optional<int> result;
  while (std::fgets(line, sizeof(line), f)) {
    // Split off the "iface:" prefix; the rest is whitespace-separated numeric columns.
    char* colon = std::strchr(line, ':');
    if (!colon) continue;
    // Columns after the colon: status, link, level, noise, ...
    // Parse the 3rd whitespace-separated token as the level (dBm).
    char* p = colon + 1;
    int col = 0;
    int level = 0;
    bool got = false;
    while (*p) {
      while (*p == ' ' || *p == '\t') ++p;
      if (!*p || *p == '\n') break;
      char* start = p;
      while (*p && *p != ' ' && *p != '\t' && *p != '\n') ++p;
      ++col;
      if (col == 3) {  // level column
        std::string tok(start, p - start);
        try {
          level = std::stoi(tok);  // stops at the trailing '.'
          got = true;
        } catch (...) {
          got = false;
        }
        break;
      }
    }
    if (got && level < 0) {  // plausible dBm reading → this interface is associated
      result = level;
      break;
    }
  }

  std::fclose(f);
  return result;
}

}  // namespace nexus::streamer::net
