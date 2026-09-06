#pragma once

#include <atomic>
#include <thread>

#include "audio/IAudioSource.h"

namespace nexus::audio {

// Real audio source: a UDP socket bound to the sync port, receiving datagrams on its own thread.
// Built only when NEXUS_STUB_HAL is off.
class UdpAudioSource : public IAudioSource {
 public:
  ~UdpAudioSource() override;

  core::Status start(int port, PacketHandler handler) override;
  core::Status stop() override;

 private:
  void recvLoop();

  int fd_ = -1;
  PacketHandler handler_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace nexus::audio
