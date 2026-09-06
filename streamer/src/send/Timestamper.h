#pragma once

#include <cstdint>

namespace nexus::streamer::send {

// Assigns absolute future playback timestamps (epoch seconds) to successive audio blocks so all
// speakers release each block at the same wall-clock instant → sample-aligned multi-room.
//
// Strategy (see the plan's timestamping section): the FIRST block is stamped at now + lead; every
// subsequent block advances on the AUDIO clock (block_frames / sample_rate), NOT on the wall clock,
// so capture-scheduling jitter never leaks into the timeline. `lead` must be >= the speaker's
// StreamSync target (~80 ms) plus a WiFi jitter margin.
//
// The wall clock is injected (a callable returning epoch seconds) so the sender is deterministic
// and unit-testable off-target — the same pattern the speaker uses for its clock in StreamSync.
class Timestamper {
 public:
  Timestamper(int sample_rate, int block_frames, double lead_seconds)
      : block_seconds_(static_cast<double>(block_frames) / static_cast<double>(sample_rate)),
        lead_seconds_(lead_seconds) {}

  // Start (or restart) a stream anchored at `now_epoch`. Call on START_AUDIO / after a resync.
  void start(double now_epoch) {
    next_timestamp_ = now_epoch + lead_seconds_;
    started_ = true;
  }

  bool started() const { return started_; }

  // Timestamp for the next block, then advance by exactly one block on the audio clock.
  double nextTimestamp() {
    const double t = next_timestamp_;
    next_timestamp_ += block_seconds_;
    return t;
  }

  double blockSeconds() const { return block_seconds_; }

 private:
  double block_seconds_;
  double lead_seconds_;
  double next_timestamp_ = 0.0;
  bool started_ = false;
};

}  // namespace nexus::streamer::send
