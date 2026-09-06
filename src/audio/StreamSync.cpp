#include "audio/StreamSync.h"

namespace nexus::audio {

namespace {
constexpr double kSmoothing = 0.1;  // EWMA factor for the offset estimate
}

void StreamSync::observe(double packet_timestamp, double now) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Offset between the packet's intended play time and arrival. A positive value means the packet
  // arrived early (buffered ahead); negative means it's late.
  const double offset_ms = (packet_timestamp - now) * 1000.0;

  if (!have_offset_) {
    stats_.clock_offset_ms = offset_ms;
    have_offset_ = true;
  } else {
    // Exponentially-weighted moving average to smooth jitter.
    const double prev = stats_.clock_offset_ms;
    stats_.clock_offset_ms = prev + kSmoothing * (offset_ms - prev);
    // Crude drift estimate from the change in offset per observation.
    stats_.drift_ppm = (stats_.clock_offset_ms - prev) * 1000.0;
  }
  // Latency proxy: how late a packet is relative to its timestamp, floored at 0.
  stats_.latency_ms = offset_ms < 0 ? -offset_ms : 0.0;
  last_offset_ms_ = offset_ms;
  ++stats_.observations;
}

double StreamSync::msUntilDue(double packet_timestamp, double now) const {
  // Play a little before the exact timestamp to account for output latency (target buffer).
  return (packet_timestamp - now) * 1000.0 - target_buffer_ms_;
}

bool StreamSync::due(double packet_timestamp, double now) const {
  return msUntilDue(packet_timestamp, now) <= 0.0;
}

SyncStats StreamSync::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void StreamSync::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_ = SyncStats{};
  have_offset_ = false;
  last_offset_ms_ = 0.0;
}

}  // namespace nexus::audio
