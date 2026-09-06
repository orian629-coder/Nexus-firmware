#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace nexus::streamer::sources {

// Single-quote a string for safe use as one shell argument, neutralizing embedded quotes. Both a
// file path (SourceFactory) and a capture device id reach a shell, and a name containing a quote
// would otherwise break out of the argument and run as a command. macOS device names are
// user-editable, so this is reachable input, not a theoretical case.
inline std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

// One capture device the streamer can stream FROM: a virtual loopback (BlackHole on macOS, a
// PipeWire/ALSA loopback on the Pi) or a real input.
//
// `id` is what gets persisted and passed back in a SourceSpec; `name` is only for display. They are
// separate because a user can rename an audio device in the OS, and a config that stored the display
// name would silently stop resolving. On macOS `id` is the CoreAudio UID; on Linux it is the
// PipeWire/ALSA node name.
struct CaptureDevice {
  std::string id;
  std::string name;
  bool loopback = false;  // true if this device carries system audio (what the user usually wants)
};

// Does this device name look like a system-audio loopback rather than a physical input?
//
// Pure and header-inline so it is testable on any host, including CI machines with no audio backend
// at all. It only drives which entry the UI highlights as the recommended source — a wrong guess
// costs a hint, never the ability to select the device.
inline bool isLoopbackName(const std::string& name) {
  // Lowercase ASCII compare; device names may be non-ASCII (macOS localizes them), and those bytes
  // pass through untouched rather than being mangled by a locale-dependent tolower.
  std::string lower;
  lower.reserve(name.size());
  for (char raw : name) {
    const unsigned char c = static_cast<unsigned char>(raw);
    lower.push_back(c < 0x80 ? static_cast<char>(std::tolower(c)) : raw);
  }
  for (const char* needle : {"blackhole", "loopback", "monitor", "soundflower", "virtual"}) {
    if (lower.find(needle) != std::string::npos) return true;
  }
  return false;
}

// Enumerate capture devices on this host. Returns an empty list (never an error) when no capture
// backend is present — a machine with no way to capture is a normal state, not a failure, and the
// UI shows "no devices" rather than an error banner.
//
// Blocking: spawns a short-lived helper process. Called from the web thread only, never from the
// audio thread.
std::vector<CaptureDevice> listCaptureDevices();

// Build the shell command that emits raw s16le/48k/stereo PCM on stdout for `device_id`.
// Empty string means this platform has no capture backend available.
//
// This is the ONE place platform capture differs, which is what keeps the requirement "do not make
// the core depend on BlackHole" enforceable: the engine, the API and the config all speak
// CaptureDevice, and only this function knows what a device is on this OS.
std::string captureCommandFor(const std::string& device_id);

}  // namespace nexus::streamer::sources
