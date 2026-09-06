#include "discovery/NetworkScanner.h"

#include <arpa/inet.h>
#include <httplib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace nexus::streamer::discovery {

namespace {

// Derive the /24 base ("192.168.1.") from a CIDR or a plain IP. Returns empty on parse failure.
std::string subnetBase(const std::string& cidr_or_ip) {
  // Take the part before '/', then strip the last octet.
  std::string ip = cidr_or_ip;
  if (auto slash = ip.find('/'); slash != std::string::npos) ip = ip.substr(0, slash);
  auto last_dot = ip.rfind('.');
  if (last_dot == std::string::npos) return "";
  return ip.substr(0, last_dot + 1);  // includes trailing dot, e.g. "192.168.1."
}

// Probe one host. Returns a filled ScannedSpeaker via `out` and true if it looks like a nexus
// speaker (status parsed and has a device_id).
bool probe(const std::string& ip, int port, int timeout_ms, ScannedSpeaker& out) {
  httplib::Client cli(ip, port);
  cli.set_connection_timeout(0, timeout_ms * 1000);  // sec, usec
  cli.set_read_timeout(0, timeout_ms * 1000);
  cli.set_write_timeout(0, timeout_ms * 1000);
  auto res = cli.Get("/api/status");
  if (!res || res->status != 200) return false;
  try {
    auto j = nlohmann::json::parse(res->body);
    const std::string device_id = j.value("device_id", "");
    if (device_id.empty()) return false;  // not a nexus speaker
    out.ip = ip;
    out.device_id = device_id;
    out.state = j.value("state", "");
    out.software_version = j.value("software_version", "");
    out.paired = j.value("paired", false);
    out.setup_mode = j.value("setup_mode", false);
    out.box_public_key = j.value("box_public_key", "");
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

std::vector<ScannedSpeaker> scanSubnet(const std::string& cidr_or_ip, int port, int timeout_ms,
                                       int concurrency) {
  std::vector<ScannedSpeaker> found;
  std::mutex found_mtx;
  const std::string base = subnetBase(cidr_or_ip);
  if (base.empty()) return found;

  // Work queue: host octets 1..254. A pool of workers drains it concurrently so a /24 finishes in
  // ~timeout * (254/concurrency) instead of serially.
  std::atomic<int> next{1};
  auto worker = [&]() {
    for (;;) {
      int host = next.fetch_add(1);
      if (host > 254) return;
      ScannedSpeaker s;
      if (probe(base + std::to_string(host), port, timeout_ms, s)) {
        std::lock_guard<std::mutex> lk(found_mtx);
        found.push_back(std::move(s));
      }
    }
  };

  const int n = concurrency < 1 ? 1 : (concurrency > 254 ? 254 : concurrency);
  std::vector<std::thread> pool;
  pool.reserve(n);
  for (int i = 0; i < n; ++i) pool.emplace_back(worker);
  for (auto& t : pool) t.join();
  return found;
}

std::string localSubnetOr(const std::string& fallback) {
  // "Connect" a UDP socket to a routable address (no packets sent) so the OS picks the outbound
  // interface, then read that socket's local IPv4 and derive its /24.
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return fallback;
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(53);
  ::inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);
  std::string result = fallback;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
      char buf[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
        std::string ip(buf);
        if (auto last = ip.rfind('.'); last != std::string::npos) {
          result = ip.substr(0, last) + ".0/24";
        }
      }
    }
  }
  ::close(fd);
  return result;
}

}  // namespace nexus::streamer::discovery
