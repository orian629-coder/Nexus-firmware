#include "provisioning/ProvisioningWindow.h"
#include <algorithm>
namespace nexus::streamer::provisioning {
void ProvisioningWindow::open(std::int64_t now_epoch, int ttl_seconds) {
  const int ttl = std::clamp(ttl_seconds, 1, kMaxTtlSeconds);
  std::lock_guard<std::mutex> lk(m_);
  open_ = true;
  expiry_ = now_epoch + ttl;
}
void ProvisioningWindow::close() {
  std::lock_guard<std::mutex> lk(m_);
  open_ = false;
  expiry_ = 0;
}
bool ProvisioningWindow::isOpen(std::int64_t now_epoch) const {
  std::lock_guard<std::mutex> lk(m_);
  return open_ && now_epoch < expiry_;
}
int ProvisioningWindow::secondsRemaining(std::int64_t now_epoch) const {
  std::lock_guard<std::mutex> lk(m_);
  if (!open_ || now_epoch >= expiry_) return 0;
  return static_cast<int>(expiry_ - now_epoch);
}
}  // namespace nexus::streamer::provisioning
