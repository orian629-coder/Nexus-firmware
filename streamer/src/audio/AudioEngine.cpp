#include "audio/AudioEngine.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "core/StreamerEvents.h"

namespace nexus::streamer::audio {

namespace {

double defaultNowEpoch() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

// Sleep in short slices so stop() is prompt even when the next block is far off. The audio thread
// must never sit in one long sleep_until, or shutdown would block for that whole interval.
constexpr auto kMaxSleepSlice = std::chrono::milliseconds(20);

}  // namespace

AudioEngine::AudioEngine(send::IPacketSink& sink, core::EventBus* bus, int sample_rate,
                         int channels, std::uint32_t block_frames, double lead_seconds, Clock clock)
    : sink_(sink),
      bus_(bus),
      sample_rate_(sample_rate),
      channels_(channels),
      block_frames_(block_frames),
      lead_seconds_(lead_seconds),
      clock_(clock ? std::move(clock) : Clock(defaultNowEpoch)),
      timestamper_(sample_rate, static_cast<int>(block_frames), lead_seconds),
      packetizer_(timestamper_, channels) {}

AudioEngine::~AudioEngine() { stop(); }

core::Status AudioEngine::start() {
  if (running_.exchange(true)) return core::Status::success();  // idempotent
  state_ = core::ServiceState::Starting;
  packets_sent_ = 0;
  thread_ = std::thread([this] { loop(); });
  state_ = core::ServiceState::Running;
  return core::Status::success();
}

core::Status AudioEngine::stop() {
  if (!running_.exchange(false)) return core::Status::success();  // idempotent
  state_ = core::ServiceState::Stopping;
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lk(source_mutex_);
    source_.reset();
  }
  transport_state_ = State::Idle;
  state_ = core::ServiceState::Stopped;
  return core::Status::success();
}

core::Status AudioEngine::play(std::unique_ptr<sources::IAudioSource> source) {
  if (!source) {
    return core::Status::error(core::ErrorCode::InvalidArg, "audio: null source");
  }
  {
    std::lock_guard<std::mutex> lk(source_mutex_);
    source_ = std::move(source);
    // Re-anchor the timeline: the first block of this stream is stamped now+lead, and every later
    // block advances on the audio clock (never the wall clock) so capture jitter cannot leak in.
    timestamper_.start(clock_());
    blocks_this_stream_ = 0;
    wall_start_ = std::chrono::steady_clock::now();
  }
  transport_state_ = State::Playing;
  if (bus_) bus_->publish(events::make(events::kStreamStarted));
  return core::Status::success();
}

void AudioEngine::pause() {
  State expected = State::Playing;
  transport_state_.compare_exchange_strong(expected, State::Paused);
}

void AudioEngine::resume() {
  State expected = State::Paused;
  if (!transport_state_.compare_exchange_strong(expected, State::Playing)) return;
  // Re-anchor so the pause duration isn't treated as a backlog to catch up on.
  std::lock_guard<std::mutex> lk(source_mutex_);
  timestamper_.start(clock_());
  blocks_this_stream_ = 0;
  wall_start_ = std::chrono::steady_clock::now();
}

void AudioEngine::stopStream() {
  {
    std::lock_guard<std::mutex> lk(source_mutex_);
    source_.reset();
  }
  // Drop the bars to silence. A meter left holding its last reading looks exactly like a live
  // signal, which is worse than showing nothing at all.
  input_meter_.reset();
  output_meter_.reset();
  if (transport_state_.exchange(State::Idle) != State::Idle && bus_) {
    bus_->publish(events::make(events::kStreamStopped));
  }
}

void AudioEngine::setDspHook(DspHook hook) {
  // Same mutex as the source: the audio thread holds it across read → dsp → pack, so the hook can
  // never be replaced midway through processing a block.
  std::lock_guard<std::mutex> lk(source_mutex_);
  dsp_hook_ = std::move(hook);
}

AudioEngine::Transport AudioEngine::transport() const {
  Transport t;
  t.state = transport_state_.load();
  t.packets_sent = packets_sent_.load();
  t.targets = targets_.load();
  return t;
}

bool AudioEngine::sendBlock() {
  std::vector<std::uint8_t> datagram;
  {
    std::lock_guard<std::mutex> lk(source_mutex_);
    if (!source_) return false;

    scratch_.clear();
    const std::size_t frames = source_->read(block_frames_, scratch_);
    if (frames == 0) {
      // A finite source is done; a live source just has nothing yet and keeps the stream open.
      return !source_->exhausted();
    }
    // Metered BEFORE the DSP: this is the raw signal the source delivered.
    input_meter_.measure(scratch_.data(), frames, channels_);

    // Master DSP runs BEFORE packetization, so every speaker receives identically processed audio
    // and per-speaker DSP on the far end composes with it rather than fighting it. In place: the
    // hook must not reallocate, and a bypassed chain costs one predictable-branch call per block.
    if (dsp_hook_) {
      dsp_hook_(scratch_.data(), frames, channels_);
    }

    // Metered AFTER the DSP, so this is exactly what is packetized and sent.
    output_meter_.measure(scratch_.data(), frames, channels_);

    const bool last = source_->exhausted();
    datagram = packetizer_.pack(scratch_.data(), static_cast<std::uint32_t>(frames), last);
    if (last) {
      sink_.send(datagram);
      packets_sent_.fetch_add(1);
      return false;
    }
  }
  // Sent outside the lock: fanning out to N speakers must not block a UI-thread source swap.
  sink_.send(datagram);
  packets_sent_.fetch_add(1);
  return true;
}

void AudioEngine::loop() {
  const double block_secs = static_cast<double>(block_frames_) / static_cast<double>(sample_rate_);

  while (running_.load()) {
    if (transport_state_.load() != State::Playing) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (!sendBlock()) {
      // End of a finite stream (or the source vanished): go idle and wait for the next play().
      if (transport_state_.exchange(State::Idle) != State::Idle) {
        // Same reason as stopStream(): a file reaching its end must drop the bars, not leave them
        // frozen at the last block of music.
        input_meter_.reset();
        output_meter_.reset();
        if (bus_) bus_->publish(events::make(events::kStreamStopped));
      }
      continue;
    }

    // Pace on the wall clock anchored at stream start rather than accumulating sleeps, so send-rate
    // drift can't build up and starve or overflow the speaker's jitter buffer.
    ++blocks_this_stream_;
    const auto due = wall_start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(blocks_this_stream_ * block_secs));
    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= due) break;
      std::this_thread::sleep_for(std::min(
          kMaxSleepSlice, std::chrono::duration_cast<std::chrono::milliseconds>(due - now) +
                              std::chrono::milliseconds(1)));
    }
  }
}

}  // namespace nexus::streamer::audio
