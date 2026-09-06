#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "audio/AudioPacket.h"

namespace nexus::audio {

// Runtime metrics for the jitter buffer, surfaced in telemetry.
struct BufferMetrics {
  std::size_t depth = 0;            // packets currently queued
  std::uint64_t received = 0;
  std::uint64_t dropped_overflow = 0;
  std::uint64_t lost = 0;          // gaps inferred from sequence numbers
  std::uint64_t underflows = 0;
  std::uint64_t last_sequence = 0;
};

// Jitter buffer between the network receiver and playback. Ordered by sequence, it absorbs network
// timing variance: it prefills before playback starts, detects packet loss (sequence gaps),
// underflow (empty on pull), and overflow (drops oldest when full). Thread-safe: the receiver
// pushes and the playback thread pops.
class AudioBuffer {
 public:
  AudioBuffer(std::size_t prefill = 10, std::size_t capacity = 50)
      : prefill_(prefill), capacity_(capacity) {}

  // Insert a packet. Drops the oldest packet on overflow. Updates loss/received metrics.
  void push(AudioPacket pkt);

  // Pop the next packet for playback, or nullopt on underflow (empty). Counts underflows.
  std::optional<AudioPacket> pop();

  // Target playback timestamp of the packet at the FRONT, without removing it. nullopt when empty.
  //
  // This is what makes timestamp-honouring playback possible: the decision "is it time yet" has to
  // happen BEFORE the packet leaves the queue. Popping first and discovering the packet is early
  // leaves no way to put it back, which is why playback previously had to play everything
  // immediately and could not stay aligned across speakers.
  //
  // Deliberately not counted as an underflow when empty — peeking is a question, not a read.
  std::optional<double> peekTimestamp() const;

  // True when the queue is close enough to capacity that holding another packet back would start
  // costing audio. Timestamp-honouring playback uses this as a safety valve: waiting is only viable
  // while the queue can still store what is being held, and past that point overflow silently
  // destroys the tail of the stream to preserve a schedule that can no longer be met.
  //
  // The 80% threshold leaves headroom for a burst to land while playback is deciding, rather than
  // triggering exactly at the cliff edge.
  bool nearlyFull() const;

  // True once enough packets are queued to begin playback (>= prefill).
  bool ready() const;

  std::size_t depth() const;
  BufferMetrics metrics() const;
  void reset();

 private:
  mutable std::mutex mutex_;
  std::deque<AudioPacket> queue_;
  std::size_t prefill_;
  std::size_t capacity_;
  BufferMetrics metrics_;
  bool have_last_seq_ = false;
};

}  // namespace nexus::audio
