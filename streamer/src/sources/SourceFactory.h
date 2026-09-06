#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Result.h"
#include "sources/CaptureDevices.h"
#include "sources/IAudioSource.h"
#include "sources/ProcessPcmSource.h"
#include "sources/StdinPcmSource.h"
#include "sources/WavFileSource.h"

namespace nexus::streamer::sources {

// What to play.
struct SourceSpec {
  std::string kind;  // "file" | "stdin" | "process" | "device"
  std::string uri;   // path for file, command for process, device id for device; ignored for stdin
};

// The ONE place a source is constructed. AudioEngine only ever sees IAudioSource, so adding
// Bluetooth or AirPlay later means adding a case here and nothing else — no change to the engine,
// the app, or the API. That is the point of routing every construction through one function.
//
// Escaping note: a file path is passed to a shell for the "process" kind, so it is single-quoted
// with embedded quotes neutralized. Without that, a filename containing a quote would let the path
// break out of the argument and run as a command. Defined in CaptureDevices.h, which needs the same
// escaping for device ids and is the leaf header of the two.

inline core::Result<std::unique_ptr<IAudioSource>> makeSource(const SourceSpec& spec) {
  if (spec.kind == "stdin") {
    return std::unique_ptr<IAudioSource>(new StdinPcmSource(stdin, 2));
  }

  if (spec.kind == "file") {
    if (spec.uri.empty()) {
      return core::Status::error(core::ErrorCode::InvalidArg, "file source needs a path");
    }
    // Plain WAV is decoded in-process; anything else needs a decoder, so fall back to ffmpeg.
    auto wav = WavFileSource::open(spec.uri);
    if (wav.ok()) return std::unique_ptr<IAudioSource>(std::move(wav.value()));

    const std::string cmd = "ffmpeg -v quiet -i " + shellQuote(spec.uri) +
                            " -ar 48000 -ac 2 -f s16le -";
    auto proc = ProcessPcmSource::spawn(cmd, /*live=*/false);
    if (!proc.ok()) {
      // Report the WAV parse failure too: if ffmpeg is missing, "cannot start ffmpeg" alone is a
      // confusing answer to "why won't this .wav play".
      return core::Status::error(core::ErrorCode::InvalidArg,
                                 "cannot play " + spec.uri + " (" + wav.status().message() +
                                     "; ffmpeg fallback also failed)");
    }
    return std::unique_ptr<IAudioSource>(std::move(proc.value()));
  }

  if (spec.kind == "process") {
    if (spec.uri.empty()) {
      return core::Status::error(core::ErrorCode::InvalidArg, "process source needs a command");
    }
    auto proc = ProcessPcmSource::spawn(spec.uri, /*live=*/true);
    if (!proc.ok()) return proc.status();
    return std::unique_ptr<IAudioSource>(std::move(proc.value()));
  }

  // A capture device (BlackHole on macOS, a PipeWire node on the Pi). The platform difference lives
  // entirely in captureCommandFor(); from here on it is just another live process source, which is
  // why "play what my Mac is playing" needed no change to the engine.
  if (spec.kind == "device") {
    if (spec.uri.empty()) {
      return core::Status::error(core::ErrorCode::InvalidArg, "device source needs a device id");
    }
    const std::string cmd = captureCommandFor(spec.uri);
    if (cmd.empty()) {
      return core::Status::error(core::ErrorCode::NotImplemented,
                                 "no capture backend on this host (see docs: maccapture/pw-record)");
    }
    auto proc = ProcessPcmSource::spawn(cmd, /*live=*/true);
    if (!proc.ok()) return proc.status();
    return std::unique_ptr<IAudioSource>(std::move(proc.value()));
  }

  return core::Status::error(core::ErrorCode::InvalidArg, "unknown source kind: " + spec.kind);
}

// Kinds this build can actually play, for GET /api/sources. "device" is listed unconditionally: the
// kind is always understood, and whether any device exists is answered by the device list itself.
inline std::vector<std::string> availableKinds() {
  return {"file", "stdin", "process", "device"};
}

}  // namespace nexus::streamer::sources
