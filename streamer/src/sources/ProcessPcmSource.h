#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Result.h"
#include "sources/StdinPcmSource.h"

namespace nexus::streamer::sources {

// Reads raw PCM from a child process's stdout — the same read loop as StdinPcmSource, just pointed
// at a popen() pipe instead of stdin.
//
// This is what makes the streamer format-agnostic without linking a decoder: anything ffmpeg can
// open (FLAC, MP3, AAC, an HTTP URL, a capture device) becomes a source. WavFileSource stays the
// zero-dependency path for plain WAV; this covers everything else, on machines where ffmpeg exists.
//
// The child MUST be reaped: a streamer that starts and stops playback repeatedly would otherwise
// leak an ffmpeg process per track until the box runs out of PIDs. pclose() in the destructor is the
// whole fix, and it also sends EOF so the child exits on its own when we stop reading.
class ProcessPcmSource : public IAudioSource {
 public:
  // `command` must emit raw s16le/48k/stereo on stdout, e.g.
  //   ffmpeg -v quiet -i "song.flac" -ar 48000 -ac 2 -f s16le -
  static core::Result<std::unique_ptr<ProcessPcmSource>> spawn(const std::string& command,
                                                               bool live = false) {
    std::FILE* p = ::popen(command.c_str(), "r");
    if (!p) {
      return core::Status::error(core::ErrorCode::IoError, "failed to start source process");
    }
    return std::unique_ptr<ProcessPcmSource>(new ProcessPcmSource(p, live));
  }

  ~ProcessPcmSource() override { close(); }

  std::size_t read(std::size_t max_frames, std::vector<std::int16_t>& out) override {
    if (!pipe_) return 0;
    return inner_.read(max_frames, out);
  }

  // A live source never reports exhaustion: a momentary gap in a capture stream is not the end of
  // the stream, and treating it as one would stop playback on the first hiccup.
  bool exhausted() const override { return live_ ? false : inner_.exhausted(); }

  bool live() const override { return live_; }

  void close() {
    if (pipe_) {
      ::pclose(pipe_);  // also reaps the child
      pipe_ = nullptr;
    }
  }

 private:
  ProcessPcmSource(std::FILE* pipe, bool live)
      : pipe_(pipe), live_(live), inner_(pipe, 2) {}

  std::FILE* pipe_ = nullptr;
  bool live_ = false;
  StdinPcmSource inner_;  // reuses the existing whole-frame read logic
};

}  // namespace nexus::streamer::sources
