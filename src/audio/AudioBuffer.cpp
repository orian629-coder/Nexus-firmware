#include "audio/AudioBuffer.h"

namespace nexus::audio {

void AudioBuffer::push(AudioPacket pkt) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++metrics_.received;

  // Infer loss from sequence gaps (only forward gaps; reordered/late packets aren't counted twice).
  if (have_last_seq_ && pkt.header.sequence > metrics_.last_sequence + 1) {
    metrics_.lost += pkt.header.sequence - metrics_.last_sequence - 1;
  }
  if (!have_last_seq_ || pkt.header.sequence > metrics_.last_sequence) {
    metrics_.last_sequence = pkt.header.sequence;
    have_last_seq_ = true;
  }

  if (queue_.size() >= capacity_) {
    queue_.pop_front();  // overflow: drop oldest to bound latency
    ++metrics_.dropped_overflow;
  }
  queue_.push_back(std::move(pkt));
}

std::optional<AudioPacket> AudioBuffer::pop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    ++metrics_.underflows;
    return std::nullopt;
  }
  AudioPacket p = std::move(queue_.front());
  queue_.pop_front();
  return p;
}

std::optional<double> AudioBuffer::peekTimestamp() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return std::nullopt;
  return queue_.front().header.timestamp;
}

bool AudioBuffer::nearlyFull() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // 80% of capacity, computed without floating point so it is exact at any capacity.
  return queue_.size() * 5 >= capacity_ * 4;
}

bool AudioBuffer::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() >= prefill_;
}

std::size_t AudioBuffer::depth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

BufferMetrics AudioBuffer::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  BufferMetrics m = metrics_;
  m.depth = queue_.size();
  return m;
}

void AudioBuffer::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
  metrics_ = BufferMetrics{};
  have_last_seq_ = false;
}

}  // namespace nexus::audio
