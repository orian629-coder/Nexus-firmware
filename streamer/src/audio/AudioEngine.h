#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/LevelMeter.h"
#include "core/EventBus.h"
#include "core/IService.h"
#include "core/Result.h"
#include "send/IPacketSink.h"
#include "send/Packetizer.h"
#include "send/Timestamper.h"
#include "sources/IAudioSource.h"

namespace nexus::streamer::audio {

// Where one datagram goes. Resolved from a speaker's host at fan-out time.
struct Endpoint {
  std::string host;
  int port = 0;
};

// The streamer's audio sender, as a long-running service.
//
// This is what unifies "serve" and "stream": before Phase 1 the paced send loop lived inside
// main() and blocked, so a process serving the control UI could never also send audio — the UI's
// play button sent a START_AUDIO command to a speaker that then received no packets. Here the loop
// owns a thread, so the web thread can swap the source or the target set while it runs.
//
// Deliberately knows nothing about HTTP, speakers, or commands: it takes an IAudioSource and a set
// of endpoints. The CMake target links only nexus_streamer_send + nexus_core (never the web lib),
// so "the audio module does not depend on the interface" is a link error rather than a convention.
class AudioEngine : public core::IService {
 public:
  enum class State { Idle, Playing, Paused };

  struct Transport {
    State state = State::Idle;
    std::uint64_t packets_sent = 0;
    std::size_t targets = 0;
  };

  // `clock` returns epoch seconds (injected for deterministic tests). `sink` is borrowed and must
  // outlive the engine — production passes a UdpPacketSink, tests a MemoryPacketSink.
  using Clock = std::function<double()>;

  AudioEngine(send::IPacketSink& sink, core::EventBus* bus = nullptr, int sample_rate = 48000,
              int channels = 2, std::uint32_t block_frames = 480, double lead_seconds = 0.18,
              Clock clock = nullptr);
  ~AudioEngine() override;

  std::string name() const override { return "audio-engine"; }
  core::Status start() override;  // spawns the thread; the engine starts Idle (no audio yet)
  core::Status stop() override;
  core::ServiceState state() const override { return state_; }

  // Master DSP applied to every block before packetization, so all speakers receive the same
  // processed audio. A std::function rather than a dsp::DspEngine member on purpose: the audio
  // module must not link the dsp library (see the CMake module-boundary note), and this keeps the
  // engine testable with a plain lambda.
  //
  // Called on the audio thread, once per block, with interleaved int16 in place.
  using DspHook = std::function<void(std::int16_t*, std::size_t, int)>;

  // Set (or clear, with nullptr) the master DSP. Safe to call while streaming: the hook is swapped
  // under the same mutex that guards the source, so a block is never processed by a half-installed
  // hook.
  void setDspHook(DspHook hook);

  // Begin streaming `source`, replacing whatever was playing. Re-anchors the timeline to now+lead.
  core::Status play(std::unique_ptr<sources::IAudioSource> source);

  // How many speakers the sink is currently fanning out to. The engine does not own the target set
  // (the sink does), so the app reports it here after retargeting — without this, Transport::targets
  // was declared but never assigned and always read 0.
  void setTargetCount(std::size_t n) { targets_.store(n); }

  void pause();
  void resume();

  // Stop streaming and drop the source. The service keeps running (ready to play again).
  void stopStream();

  Transport transport() const;

  // Signal levels around the master DSP: `input` is what the source produced, `output` is what
  // actually goes on the wire. Two meters rather than one because that pair is what makes a problem
  // diagnosable — signal at the input but silence at the output means the chain killed it, and
  // silence at both means the source is dead.
  const LevelMeter& inputMeter() const { return input_meter_; }
  const LevelMeter& outputMeter() const { return output_meter_; }

 private:
  void loop();
  // Emits one block. Returns false when a finite source has ended.
  bool sendBlock();

  send::IPacketSink& sink_;
  core::EventBus* bus_;
  const int sample_rate_;
  const int channels_;
  const std::uint32_t block_frames_;
  const double lead_seconds_;
  Clock clock_;

  std::atomic<core::ServiceState> state_{core::ServiceState::Stopped};
  std::atomic<State> transport_state_{State::Idle};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> packets_sent_{0};
  std::atomic<std::size_t> targets_{0};

  std::thread thread_;

  // Guards the source + timestamper, which the web thread swaps under the audio thread.
  mutable std::mutex source_mutex_;
  std::unique_ptr<sources::IAudioSource> source_;
  DspHook dsp_hook_;
  // Not guarded by source_mutex_: both are lock-free, so the web thread reads them without ever
  // contending with the audio thread for the source lock.
  LevelMeter input_meter_;
  LevelMeter output_meter_;
  send::Timestamper timestamper_;
  send::Packetizer packetizer_;
  std::vector<std::int16_t> scratch_;

  // Pacing anchor, owned by the audio thread alone.
  std::chrono::steady_clock::time_point wall_start_{};
  std::uint64_t blocks_this_stream_ = 0;
};

}  // namespace nexus::streamer::audio
