#include "sources/CaptureDevices.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef __APPLE__
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#endif

#include <nlohmann/json.hpp>

#include "sources/SourceFactory.h"  // shellQuote

namespace nexus::streamer::sources {
namespace {

// Run a command and return its stdout. Empty on failure — callers treat "no output" and "no backend"
// identically, since both mean there is nothing to offer the user.
std::string runCapture(const std::string& cmd) {
  std::FILE* p = ::popen(cmd.c_str(), "r");
  if (!p) return {};
  std::string out;
  std::array<char, 4096> buf{};
  while (std::size_t n = std::fread(buf.data(), 1, buf.size(), p)) out.append(buf.data(), n);
  ::pclose(p);
  return out;
}

#ifdef __APPLE__

bool fileExists(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  std::fclose(f);
  return true;
}

// Where the macOS capture helper lives. It is built by hand (swiftc -O maccapture.swift), not by
// CMake, so it can legitimately be absent — an unbuilt helper must degrade to "no devices", never to
// a crash or a bogus device list.
//
// The helper is looked up next to this executable first. Inside the .app bundle that is the only
// copy that exists, and it is also the copy that inherits the bundle's microphone entitlement —
// resolving to the source tree instead yields a binary macOS will silently deny capture to.
std::string helperPath() {
  if (const char* env = std::getenv("NEXUS_MACCAPTURE"); env && *env) return env;

  std::array<char, 4096> buf{};
  std::uint32_t size = buf.size();
  if (_NSGetExecutablePath(buf.data(), &size) == 0) {
    const std::string exe(buf.data());
    const std::size_t slash = exe.find_last_of('/');
    if (slash != std::string::npos) {
      const std::string sibling = exe.substr(0, slash + 1) + "maccapture";
      if (fileExists(sibling)) return sibling;
    }
  }

  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/nexus-speaker/streamer/mac/maccapture";
  }
  return "maccapture";
}

bool helperExists() { return fileExists(helperPath()); }

#endif

}  // namespace

std::vector<CaptureDevice> listCaptureDevices() {
  std::vector<CaptureDevice> devices;

#ifdef __APPLE__
  if (!helperExists()) return devices;

  // Line-delimited JSON: one object per device. A malformed line is skipped rather than aborting the
  // enumeration, so one odd device name cannot hide every other device from the user.
  const std::string out = runCapture(shellQuote(helperPath()) + " --list 2>/dev/null");
  std::size_t start = 0;
  while (start < out.size()) {
    const std::size_t nl = out.find('\n', start);
    const std::string line =
        out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    start = (nl == std::string::npos) ? out.size() : nl + 1;
    if (line.empty()) continue;

    auto parsed = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) continue;

    CaptureDevice d;
    d.name = parsed.value("name", std::string());
    d.id = parsed.value("uid", std::string());
    if (d.id.empty() || d.name.empty()) continue;
    d.loopback = isLoopbackName(d.name);
    devices.push_back(std::move(d));
  }
#else
  // Linux (the Pi): PipeWire monitor sources carry system audio. `pw-cli` output is parsed loosely
  // because its format is not a stable contract; a parse miss yields no devices rather than a wrong
  // one. ALSA loopback remains available by configuring a "process" source explicitly.
  const std::string out = runCapture("pw-cli ls Node 2>/dev/null");
  std::size_t start = 0;
  while (start < out.size()) {
    const std::size_t nl = out.find('\n', start);
    std::string line = out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    start = (nl == std::string::npos) ? out.size() : nl + 1;

    const std::size_t key = line.find("node.name = ");
    if (key == std::string::npos) continue;
    std::string name = line.substr(key + 12);
    // Values are quoted; strip the quotes without assuming they are present.
    if (!name.empty() && name.front() == '"') name.erase(name.begin());
    if (!name.empty() && name.back() == '"') name.pop_back();
    if (name.empty()) continue;

    CaptureDevice d;
    d.id = name;
    d.name = name;
    d.loopback = isLoopbackName(name);
    devices.push_back(std::move(d));
  }
#endif

  return devices;
}

std::string captureCommandFor(const std::string& device_id) {
  if (device_id.empty()) return {};

#ifdef __APPLE__
  if (!helperExists()) return {};
  return shellQuote(helperPath()) + " " + shellQuote(device_id);
#else
  // pw-record emits exactly the wire format, so no conversion step is needed.
  return "pw-record --target " + shellQuote(device_id) +
         " --rate 48000 --channels 2 --format s16 - 2>/dev/null";
#endif
}

}  // namespace nexus::streamer::sources
