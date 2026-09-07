#pragma once
#include <cstdint>
#include <mutex>
namespace nexus::streamer::provisioning {
class ProvisioningWindow {
 public:
  static constexpr int kMaxTtlSeconds = 1800;
  void open(std::int64_t now_epoch, int ttl_seconds);
  void close();
  bool isOpen(std::int64_t now_epoch) const;
  int secondsRemaining(std::int64_t now_epoch) const;
 private:
  mutable std::mutex m_;
  bool open_ = false;
  std::int64_t expiry_ = 0;
};
}  // namespace nexus::streamer::provisioning
