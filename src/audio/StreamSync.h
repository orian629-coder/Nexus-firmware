#pragma once

#include <cstdint>
#include <mutex>

namespace nexus::audio {

struct SyncStats {
  double clock_offset_ms = 0.0;  // estimated local - streamer clock, smoothed
  double drift_ppm = 0.0;        // clock drift estimate
  double latency_ms = 0.0;       // one-way network latency estimate
  std::uint64_t observations = 0;
};

// Tracks the relationship between the Streamer's clock (packet timestamps) and the local clock so
// playback releases each packet at its intended wall-clock moment. Relies on OS-level NTP for the
// baseline; this smooths the residual offset and decides when a packet is "due".
//
// All times are epoch seconds (double). `now` is injected so the module is deterministic in tests
// and avoids hidden clock calls.
class StreamSync {
 public:
  explicit StreamSync(double target_buffer_ms = 80.0) : target_buffer_ms_(target_buffer_ms) {}

  // Feed an observation: a packet with target `timestamp` arrived at local time `now`.
  void observe(double packet_timestamp, double now);

  // Should a packet with this target timestamp be played now? True once now >= timestamp (minus a
  // small guard). Packets far in the past are also "due" (play immediately to catch up).
  bool due(double packet_timestamp, double now) const;

  // How long (ms) until the packet is due; <= 0 means play now.
  double msUntilDue(double packet_timestamp, double now) const;

  SyncStats stats() const;
  void reset();

 private:
  mutable std::mutex mutex_;
  double target_buffer_ms_;
  SyncStats stats_;
  double last_offset_ms_ = 0.0;
  bool have_offset_ = false;
};

}  // namespace nexus::audio
