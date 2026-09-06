# Streamer-as-AP — Phase A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A paired speaker automatically joins its streamer's private WPA2 AP (`Nexus-<streamer_id>`) so mDNS discovery runs on a clean, streamer-controlled subnet instead of the venue WiFi that drops client-to-client multicast.

**Architecture:** One shared, deterministic credential-derivation module (`nexus::identity::deriveApCredentials`) lives in the `nexus_identity` library that **both** the speaker and streamer already link, so the two sides compute the identical SSID/passphrase from the `streamer_id` — this is a wire contract. The streamer hosts a permanent NetworkManager `method=shared` WPA2 AP (a script + systemd unit modeled on the existing `scripts/hotspot.sh`, with the SSID/passphrase printed by `nexus-streamer --ap-credentials` so the KDF stays in one place). The speaker, on entering `CONNECTING_NETWORK`, derives its paired streamer's SSID, finds it in a scan, and joins via the existing `NetworkManager`/`INetworkHal`. The discovery wire contract and identity generation are untouched.

**Tech Stack:** C++17, GoogleTest/CTest, CMake (`nexus_add_module`/`nexus_add_test` helpers), libsodium (`crypto::sha256Hex`), NetworkManager/`nmcli`, systemd.

**Spec:** `docs/STREAMER-AP-DESIGN.md` (read it — §2 defines the credential contract, §7 the testing gates).

> **Note — spec correction:** the spec §3 says the speaker reuses `ApScanner`/`ApJoiner`; those are actually streamer-only classes. The speaker joins WiFi through `nexus::network::NetworkManager` + `INetworkHal` (`scan()` / `connectWifi()`), which this plan uses. Update the spec line when convenient.

## Global Constraints

- **`NEXUS_STUB_HAL` defaults ON.** Host builds use the stub HAL (no real Avahi/nmcli). Device builds MUST pass `-DNEXUS_STUB_HAL=OFF`.
- **Discovery wire contract is frozen:** `_nexus-streamer._tcp` :8090, `_nexus-speaker._tcp` :45455, TCP 45455, UDP 50005. This plan does not touch it.
- **Identity generation is off-limits.** Read `streamer_id`; never regenerate or MAC/IP-derive it.
- **Do NOT run the full-Application e2e tests locally** (`nexus_e2e_test`, `EndToEnd.*`, `Soak.*`) — they shell out to `systemctl` and pop polkit GUI password dialogs. All host tests in this plan are pure unit/integration (stub HAL, no `systemctl`/`nmcli`).
- **`ctest` on PATH is a broken pyenv shim** — always use `/usr/bin/ctest`. Do a full `cmake --build build` (gtest registration goes stale after partial builds).
- **No device deploy/flash without explicit human approval** — the three Pis are client units. Tasks 5–7 are device-gated.
- **Branch:** `feat/streamer-ap`. Small, reviewed commits per task.
- **Credential KDF is a fleet-wide contract:** the salt (`nexus-audio-ap-v1:`) and passphrase length (24) must change only with a coordinated streamer+speaker rollout.

---

### Task 1: Shared AP-credential derivation module

**Files:**
- Create: `src/identity/ApCredentials.h`
- Create: `src/identity/ApCredentials.cpp`
- Modify: `src/identity/CMakeLists.txt` (add source + test target)
- Test: `tests/unit/ap_credentials_test.cpp`

**Interfaces:**
- Consumes: `nexus::identity::crypto::sha256Hex(const std::vector<std::uint8_t>&)` from `src/identity/Crypto.h`.
- Produces (relied on by Tasks 2, 3, 4):
  - `struct nexus::identity::ApCredentials { std::string ssid; std::string passphrase; };`
  - `nexus::identity::ApCredentials nexus::identity::deriveApCredentials(const std::string& streamer_id);`
  - `std::optional<std::string> nexus::identity::streamerIdFromApSsid(const std::string& ssid);`
  - `inline constexpr char nexus::identity::kApSsidPrefix[] = "Nexus-";`

- [ ] **Step 1: Create the header**

`src/identity/ApCredentials.h`:
```cpp
#pragma once

#include <optional>
#include <string>

// The private-AP naming + passphrase contract for the streamer's "Nexus-<streamer_id>" network.
// The STREAMER (which hosts the AP) and every SPEAKER (which joins it) derive these identically
// from the streamer_id, so both sides MUST agree byte-for-byte. Treat this like the mDNS wire
// contract: changing the salt, length, or SSID shape breaks how speakers join.
namespace nexus::identity {

struct ApCredentials {
  std::string ssid;        // "Nexus-<streamer_id>", e.g. "Nexus-STR-a14ad83e"
  std::string passphrase;  // deterministic WPA2 passphrase derived from streamer_id
};

// SSID prefix that marks a Nexus private AP.
inline constexpr char kApSsidPrefix[] = "Nexus-";

// Derive the AP SSID + WPA2 passphrase for a streamer id (e.g. "STR-a14ad83e").
ApCredentials deriveApCredentials(const std::string& streamer_id);

// If `ssid` is a Nexus private-AP SSID ("Nexus-STR-..."), return the embedded streamer_id;
// otherwise std::nullopt. The setup AP ("Nexus-Setup") and foreign SSIDs return nullopt.
std::optional<std::string> streamerIdFromApSsid(const std::string& ssid);

}  // namespace nexus::identity
```

- [ ] **Step 2: Write the failing test and wire CMake**

`tests/unit/ap_credentials_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "identity/ApCredentials.h"

using nexus::identity::ApCredentials;
using nexus::identity::deriveApCredentials;
using nexus::identity::streamerIdFromApSsid;

TEST(ApCredentials, SsidIsNexusPrefixPlusStreamerId) {
  const ApCredentials c = deriveApCredentials("STR-a14ad83e");
  EXPECT_EQ(c.ssid, "Nexus-STR-a14ad83e");
}

TEST(ApCredentials, DerivationIsDeterministic) {
  EXPECT_EQ(deriveApCredentials("STR-a14ad83e").passphrase,
            deriveApCredentials("STR-a14ad83e").passphrase);
}

TEST(ApCredentials, DifferentStreamersGetDifferentPassphrases) {
  EXPECT_NE(deriveApCredentials("STR-a14ad83e").passphrase,
            deriveApCredentials("STR-deadbeef").passphrase);
}

TEST(ApCredentials, PassphraseIsWpa2ValidLengthAndCharset) {
  const std::string pass = deriveApCredentials("STR-a14ad83e").passphrase;
  ASSERT_GE(pass.size(), 8u);
  ASSERT_LE(pass.size(), 63u);
  for (unsigned char ch : pass) {
    EXPECT_TRUE(std::isprint(ch)) << "non-printable char in passphrase";
  }
}

TEST(ApCredentials, StreamerIdRoundTripsFromSsid) {
  const ApCredentials c = deriveApCredentials("STR-a14ad83e");
  const auto id = streamerIdFromApSsid(c.ssid);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, "STR-a14ad83e");
}

TEST(ApCredentials, SetupApAndForeignSsidsAreNotStreamerAps) {
  EXPECT_FALSE(streamerIdFromApSsid("Nexus-Setup").has_value());
  EXPECT_FALSE(streamerIdFromApSsid("HomeWiFi").has_value());
  EXPECT_FALSE(streamerIdFromApSsid("").has_value());
}
```

In `src/identity/CMakeLists.txt`, add the test executable right after the existing `nexus_crypto_test` block (inside the file, following the same pattern):
```cmake
if(NEXUS_BUILD_TESTS)
  add_executable(nexus_ap_credentials_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/ap_credentials_test.cpp)
  target_link_libraries(nexus_ap_credentials_test PRIVATE nexus_identity GTest::gtest GTest::gtest_main)
  set_target_properties(nexus_ap_credentials_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  gtest_discover_tests(nexus_ap_credentials_test)
endif()
```

- [ ] **Step 3: Build and confirm it fails (RED)**

Run:
```bash
cmake --preset host-debug
cmake --build build
```
Expected: **link failure** — `undefined reference to nexus::identity::deriveApCredentials(...)` / `streamerIdFromApSsid(...)` (the header exists, the implementation does not yet).

- [ ] **Step 4: Implement**

`src/identity/ApCredentials.cpp`:
```cpp
#include "identity/ApCredentials.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "identity/Crypto.h"

namespace nexus::identity {
namespace {
// Fixed salt versions the derivation. Bump the suffix only with a coordinated streamer+speaker
// rollout — it changes every AP passphrase in the fleet.
constexpr char kPassphraseSalt[] = "nexus-audio-ap-v1:";
// 24 lowercase-hex chars: comfortably inside WPA2's 8-63 printable-ASCII range.
constexpr std::size_t kPassphraseLen = 24;
constexpr char kStreamerIdPrefix[] = "STR-";
}  // namespace

ApCredentials deriveApCredentials(const std::string& streamer_id) {
  const std::string salted = std::string(kPassphraseSalt) + streamer_id;
  const std::vector<std::uint8_t> bytes(salted.begin(), salted.end());
  const std::string hex = crypto::sha256Hex(bytes);  // 64 lowercase hex chars

  ApCredentials creds;
  creds.ssid = std::string(kApSsidPrefix) + streamer_id;
  creds.passphrase = hex.substr(0, kPassphraseLen);
  return creds;
}

std::optional<std::string> streamerIdFromApSsid(const std::string& ssid) {
  const std::string prefix(kApSsidPrefix);
  if (ssid.rfind(prefix, 0) != 0) return std::nullopt;
  std::string id = ssid.substr(prefix.size());
  if (id.rfind(kStreamerIdPrefix, 0) != 0) return std::nullopt;  // excludes "Nexus-Setup"
  return id;
}

}  // namespace nexus::identity
```

Add `ApCredentials.cpp` to the `nexus_add_module(identity ... SOURCES ...)` list in `src/identity/CMakeLists.txt` (alongside `Crypto.cpp`, `KeyManager.cpp`, `DeviceIdentity.cpp`).

- [ ] **Step 5: Build and confirm it passes (GREEN)**

Run:
```bash
cmake --build build
/usr/bin/ctest --test-dir build -R ApCredentials --output-on-failure
```
Expected: 6 `ApCredentials.*` tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/identity/ApCredentials.h src/identity/ApCredentials.cpp src/identity/CMakeLists.txt tests/unit/ap_credentials_test.cpp
git commit -m "feat(identity): shared Nexus-AP credential derivation (SSID + WPA2 passphrase)"
```

---

### Task 2: Speaker join helper (`joinStreamerAp`)

**Files:**
- Create: `src/network/StreamerApJoin.h`
- Create: `src/network/StreamerApJoin.cpp`
- Modify: `src/network/CMakeLists.txt` (add sources, `nexus_identity` dep, test source)
- Test: `tests/unit/streamer_ap_join_test.cpp`

**Interfaces:**
- Consumes: `nexus::identity::deriveApCredentials` (Task 1); `nexus::network::NetworkManager::scan()`, `::isConnected()`, `::connectWifi(const std::string&, const std::string&)`, `::refresh()`; `nexus::network::INetworkHal`, `WifiNetwork{ssid, signal_dbm}`, `WifiConnectParams{ssid, psk, ...}`, `NetworkStatus{connected, mode, ...}`.
- Produces (relied on by Task 3): `nexus::core::Status nexus::network::joinStreamerAp(NetworkManager& net, const std::string& streamer_id);`

- [ ] **Step 1: Create the header**

`src/network/StreamerApJoin.h`:
```cpp
#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::network {

class NetworkManager;

// Look for the paired streamer's private AP ("Nexus-<streamer_id>") among the current WiFi scan
// results and, if present and we're not already connected, join it with the derived passphrase.
// Best-effort: NotFound (AP not in range) is a normal, non-fatal outcome the caller ignores.
//   - Ok           already connected, or the join was issued successfully
//   - InvalidArg   streamer_id is empty (speaker not paired)
//   - NotFound     the streamer AP is not in range
//   - (propagated) a scan or connect failure from the network HAL
core::Status joinStreamerAp(NetworkManager& net, const std::string& streamer_id);

}  // namespace nexus::network
```

- [ ] **Step 2: Write the failing test and wire CMake**

`tests/unit/streamer_ap_join_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/EventBus.h"
#include "identity/ApCredentials.h"
#include "network/INetworkHal.h"
#include "network/NetworkManager.h"
#include "network/StreamerApJoin.h"

using namespace nexus;
using nexus::network::INetworkHal;
using nexus::network::NetworkManager;
using nexus::network::NetworkStatus;
using nexus::network::WifiConnectParams;
using nexus::network::WifiNetwork;

namespace {
// A network HAL with a settable scan list + connectivity that records the SSID/psk of the last
// connect — enough to prove joinStreamerAp derives creds and joins the right AP.
class FakeHal : public INetworkHal {
 public:
  using INetworkHal::connectWifi;  // keep the ssid+psk convenience overload

  void setScan(std::vector<WifiNetwork> nets) {
    std::lock_guard<std::mutex> l(m_);
    scan_ = std::move(nets);
  }
  void setConnected(bool c) { std::lock_guard<std::mutex> l(m_); connected_ = c; }
  std::string lastSsid() { std::lock_guard<std::mutex> l(m_); return last_ssid_; }
  std::string lastPsk() { std::lock_guard<std::mutex> l(m_); return last_psk_; }
  int connectCalls() { std::lock_guard<std::mutex> l(m_); return connect_calls_; }

  core::Result<std::vector<WifiNetwork>> scanWifi() override {
    std::lock_guard<std::mutex> l(m_);
    return scan_;
  }
  core::Status connectWifi(const WifiConnectParams& params) override {
    std::lock_guard<std::mutex> l(m_);
    ++connect_calls_;
    last_ssid_ = params.ssid;
    last_psk_ = params.psk;
    connected_ = true;
    return core::Status::success();
  }
  core::Status disconnect() override {
    std::lock_guard<std::mutex> l(m_);
    connected_ = false;
    return core::Status::success();
  }
  core::Result<NetworkStatus> status() override {
    std::lock_guard<std::mutex> l(m_);
    NetworkStatus s;
    s.connected = connected_;
    s.mode = connected_ ? "wifi" : "";
    return s;
  }

 private:
  std::mutex m_;
  std::vector<WifiNetwork> scan_;
  bool connected_ = false;
  int connect_calls_ = 0;
  std::string last_ssid_;
  std::string last_psk_;
};
}  // namespace

TEST(JoinStreamerAp, JoinsTheApWhenInRange) {
  core::EventBus bus;
  auto hal = std::make_unique<FakeHal>();
  auto* raw = hal.get();
  const auto creds = identity::deriveApCredentials("STR-LAB01");
  raw->setScan({{creds.ssid, -40}, {"Handsome", -55}});
  NetworkManager net(&bus, std::move(hal));

  const core::Status st = network::joinStreamerAp(net, "STR-LAB01");

  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(raw->connectCalls(), 1);
  EXPECT_EQ(raw->lastSsid(), creds.ssid);
  EXPECT_EQ(raw->lastPsk(), creds.passphrase);
}

TEST(JoinStreamerAp, ReturnsNotFoundWhenApAbsent) {
  core::EventBus bus;
  auto hal = std::make_unique<FakeHal>();
  auto* raw = hal.get();
  raw->setScan({{"Handsome", -55}, {"Guest", -70}});
  NetworkManager net(&bus, std::move(hal));

  const core::Status st = network::joinStreamerAp(net, "STR-LAB01");

  EXPECT_EQ(st.code(), core::ErrorCode::NotFound);
  EXPECT_EQ(raw->connectCalls(), 0);
}

TEST(JoinStreamerAp, RejectsEmptyStreamerId) {
  core::EventBus bus;
  NetworkManager net(&bus, std::make_unique<FakeHal>());
  EXPECT_EQ(network::joinStreamerAp(net, "").code(), core::ErrorCode::InvalidArg);
}

TEST(JoinStreamerAp, NoOpWhenAlreadyConnected) {
  core::EventBus bus;
  auto hal = std::make_unique<FakeHal>();
  auto* raw = hal.get();
  raw->setConnected(true);
  NetworkManager net(&bus, std::move(hal));
  net.refresh();  // pull HAL status into the manager's cache so isConnected() is true

  const core::Status st = network::joinStreamerAp(net, "STR-LAB01");

  EXPECT_TRUE(st.ok());
  EXPECT_EQ(raw->connectCalls(), 0);
}
```

In `src/network/CMakeLists.txt`, add the source and the `nexus_identity` dependency and the new test source:
```cmake
set(NETWORK_SOURCES NetworkManager.cpp StreamerApJoin.cpp)
if(NOT NEXUS_STUB_HAL)
  list(APPEND NETWORK_SOURCES NmcliNetworkHal.cpp)
endif()

nexus_add_module(network
  SOURCES
    ${NETWORK_SOURCES}
  DEPS
    nexus_core
    nexus_logging
    nexus_identity)

nexus_add_test(network
  SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/network_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit/streamer_ap_join_test.cpp)
```

- [ ] **Step 3: Build and confirm it fails (RED)**

Run: `cmake --build build`
Expected: **link failure** — `undefined reference to nexus::network::joinStreamerAp(...)`.

- [ ] **Step 4: Implement**

`src/network/StreamerApJoin.cpp`:
```cpp
#include "network/StreamerApJoin.h"

#include "identity/ApCredentials.h"
#include "network/NetworkManager.h"

namespace nexus::network {

core::Status joinStreamerAp(NetworkManager& net, const std::string& streamer_id) {
  if (streamer_id.empty()) {
    return core::Status::error(core::ErrorCode::InvalidArg, "no paired streamer_id");
  }
  if (net.isConnected()) {
    return core::Status::success();  // already on a network; the monitor reports it
  }

  const identity::ApCredentials creds = identity::deriveApCredentials(streamer_id);

  auto scan = net.scan();
  if (!scan.ok()) return scan.status();

  bool in_range = false;
  for (const auto& n : scan.value()) {
    if (n.ssid == creds.ssid) {
      in_range = true;
      break;
    }
  }
  if (!in_range) {
    return core::Status::error(core::ErrorCode::NotFound,
                               "streamer AP not in range: " + creds.ssid);
  }
  return net.connectWifi(creds.ssid, creds.passphrase);
}

}  // namespace nexus::network
```

- [ ] **Step 5: Build and confirm it passes (GREEN)**

Run:
```bash
cmake --build build
/usr/bin/ctest --test-dir build -R "JoinStreamerAp|NetworkManager" --output-on-failure
```
Expected: the 4 `JoinStreamerAp.*` tests PASS and the existing `NetworkManager.*` tests still PASS.

- [ ] **Step 6: Commit**

```bash
git add src/network/StreamerApJoin.h src/network/StreamerApJoin.cpp src/network/CMakeLists.txt tests/unit/streamer_ap_join_test.cpp
git commit -m "feat(network): joinStreamerAp — derive + join the paired streamer's private AP"
```

---

### Task 3: Wire the speaker to auto-join on entering CONNECTING_NETWORK

**Files:**
- Modify: `src/main/Application.cpp` (add a `StateChanged` subscription; add an include)
- Test: `tests/integration/integration_test.cpp` (add one integration test + a local fake HAL)

**Interfaces:**
- Consumes: `nexus::network::joinStreamerAp` (Task 2); `nexus::identity::deriveApCredentials` (Task 1); existing `config_->get().pairing.{paired,streamer_id}`, `network_` (the `NetworkManager`), `system::toString`, `system::SystemState::ConnectingNetwork`, `core::EventType::StateChanged`.
- Produces: no new symbols; behavior — entering `CONNECTING_NETWORK` on a paired speaker issues a best-effort join of `Nexus-<streamer_id>`.

- [ ] **Step 1: Write the failing integration test**

Add these includes near the other `network/` includes at the top of `tests/integration/integration_test.cpp`:
```cpp
#include "identity/ApCredentials.h"
#include "network/INetworkHal.h"
#include "network/StreamerApJoin.h"
```

Append this test (and its local fake HAL) to `tests/integration/integration_test.cpp`:
```cpp
namespace {
// A network HAL exposing a fixed scan list and recording the last connect — lets the wiring test
// prove a paired speaker joins its streamer AP when the state machine enters CONNECTING_NETWORK.
class ApJoinFakeHal : public network::INetworkHal {
 public:
  using INetworkHal::connectWifi;
  explicit ApJoinFakeHal(std::vector<network::WifiNetwork> scan) : scan_(std::move(scan)) {}
  core::Result<std::vector<network::WifiNetwork>> scanWifi() override { return scan_; }
  core::Status connectWifi(const network::WifiConnectParams& p) override {
    last_ssid = p.ssid;
    last_psk = p.psk;
    ++calls;
    connected_ = true;
    return core::Status::success();
  }
  core::Status disconnect() override {
    connected_ = false;
    return core::Status::success();
  }
  core::Result<network::NetworkStatus> status() override {
    network::NetworkStatus s;
    s.connected = connected_;
    s.mode = connected_ ? "wifi" : "";
    return s;
  }
  std::string last_ssid;
  std::string last_psk;
  int calls = 0;

 private:
  std::vector<network::WifiNetwork> scan_;
  bool connected_ = false;
};
}  // namespace

// Phase A wiring: on entering CONNECTING_NETWORK a paired speaker must auto-JOIN its streamer's
// private AP ("Nexus-<streamer_id>"). This installs the same StateChanged->joinStreamerAp
// subscription Application.cpp wires and asserts the join lands on the derived SSID/passphrase.
TEST(Integration, PairedSpeakerJoinsStreamerApOnConnectingNetwork) {
  auto dir = sandbox("apjoin");
  core::EventBus bus;
  config::ConfigManager config((dir / "config.json").string(), &bus);
  ASSERT_TRUE(config.start().ok());
  ASSERT_TRUE(config
                  .update([](config::SpeakerConfig& c) {
                    c.pairing.paired = true;
                    c.pairing.streamer_id = "STR-LAB01";
                  })
                  .ok());

  const auto creds = identity::deriveApCredentials("STR-LAB01");
  auto hal = std::make_unique<ApJoinFakeHal>(
      std::vector<network::WifiNetwork>{{creds.ssid, -40}, {"Handsome", -60}});
  auto* raw = hal.get();
  network::NetworkManager net(&bus, std::move(hal));

  system::SystemManager sys(&bus);
  ASSERT_TRUE(sys.start().ok());

  // The wiring under test (mirrors Application::buildServices).
  bus.subscribe(core::EventType::StateChanged, [&](const core::Event& e) {
    if (e.data.value("to", "") != system::toString(system::SystemState::ConnectingNetwork)) return;
    const auto& p = config.get().pairing;
    if (!p.paired || p.streamer_id.empty()) return;
    network::joinStreamerAp(net, p.streamer_id);
  });

  sys.enterInitialState(config.get().pairing.paired);  // -> CONNECTING_NETWORK, emits StateChanged
  bus.drain();

  EXPECT_EQ(raw->calls, 1);
  EXPECT_EQ(raw->last_ssid, creds.ssid);
  EXPECT_EQ(raw->last_psk, creds.passphrase);
}
```

- [ ] **Step 2: Build and confirm the test compiles and locks the wiring (RED/baseline)**

Run:
```bash
cmake --build build
/usr/bin/ctest --test-dir build -R PairedSpeakerJoinsStreamerApOnConnectingNetwork --output-on-failure
```
Expected: it builds (once the new includes resolve) and PASSES — it exercises the *replicated* wiring + the real `joinStreamerAp`. Its job is to lock the wiring contract that Step 3 adds to the production path; if it fails to link, confirm the integration target transitively links `nexus_identity` via `nexus_network`.

- [ ] **Step 3: Add the production wiring**

In `src/main/Application.cpp`, add near the other `#include`s:
```cpp
#include "network/StreamerApJoin.h"
```

Immediately after the existing `StateChanged` subscription that starts/stops discovery on the `SearchingStreamer` edge (the `bus_.subscribe(core::EventType::StateChanged, ...)` block that reads `config_->get().pairing`), add a second subscription:
```cpp
  // On entering CONNECTING_NETWORK, a paired speaker best-effort joins its streamer's private AP
  // ("Nexus-<streamer_id>"). NotFound (AP not in range) is normal — the usual network path
  // (persisted profile / ethernet) still applies. See docs/STREAMER-AP-DESIGN.md.
  bus_.subscribe(core::EventType::StateChanged, [this](const core::Event& e) {
    if (e.data.value("to", "") != system::toString(system::SystemState::ConnectingNetwork)) return;
    const auto& p = config_->get().pairing;
    if (!p.paired || p.streamer_id.empty()) return;
    const core::Status st = network::joinStreamerAp(*network_, p.streamer_id);
    if (!st.ok() && st.code() != core::ErrorCode::NotFound) {
      NX_LOG_WARN("main", "streamer-AP join failed");
    }
  });
```

- [ ] **Step 4: Build and confirm GREEN (and no regressions)**

Run:
```bash
cmake --build build
/usr/bin/ctest --test-dir build -R "PairedSpeakerJoinsStreamerApOnConnectingNetwork|PairedSpeakerAutoDiscoversStreamerFromStateWiring" --output-on-failure
```
Expected: both integration tests PASS. Then run the full host suite excluding the local-dialog e2e tests:
```bash
/usr/bin/ctest --test-dir build --output-on-failure -E "EndToEnd|Soak"
```
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/main/Application.cpp tests/integration/integration_test.cpp
git commit -m "feat(main): paired speaker auto-joins its streamer AP on entering CONNECTING_NETWORK"
```

---

### Task 4: Streamer `--ap-credentials` CLI + contract guard

**Files:**
- Modify: `streamer/src/main/StreamerMain.cpp` (add an include + an early `--ap-credentials` branch)
- Modify: `streamer/CMakeLists.txt` (add the contract-guard test target)
- Test: `streamer/tests/unit/ap_credentials_contract_test.cpp`

**Interfaces:**
- Consumes: `nexus::streamer::identity::StreamerIdentity` (`streamerId()`, `load()`), the existing `identityKeyPath()` helper in `StreamerMain.cpp`, and `nexus::identity::deriveApCredentials` (Task 1). The streamer already links both `nexus_streamer_identity` and `nexus_identity`.
- Produces: `nexus-streamer --ap-credentials` prints two lines (`NEXUS_AP_SSID=...`, `NEXUS_AP_PASSPHRASE=...`) and exits 0; consumed by `scripts/streamer-ap.sh` (Task 5).

- [ ] **Step 1: Write the failing contract-guard test**

`streamer/tests/unit/ap_credentials_contract_test.cpp`:
```cpp
#include <gtest/gtest.h>

#include "identity/ApCredentials.h"

// Contract guard: the streamer hosts the AP each speaker joins, so it MUST derive from the same
// shared module. This proves the module links in the streamer tree and yields a WPA2-valid,
// stable SSID/passphrase for a known streamer_id.
TEST(StreamerApCredentialsContract, DerivesValidStableCredentials) {
  const auto c = nexus::identity::deriveApCredentials("STR-LAB01");
  EXPECT_EQ(c.ssid, "Nexus-STR-LAB01");
  EXPECT_EQ(c.passphrase, nexus::identity::deriveApCredentials("STR-LAB01").passphrase);
  ASSERT_GE(c.passphrase.size(), 8u);
  ASSERT_LE(c.passphrase.size(), 63u);
}
```

In `streamer/CMakeLists.txt`, inside the `if(NEXUS_BUILD_TESTS)` section, add (mirroring the `nexus_streamer_identity_test` block):
```cmake
add_executable(nexus_streamer_ap_credentials_test tests/unit/ap_credentials_contract_test.cpp)
target_include_directories(nexus_streamer_ap_credentials_test PRIVATE
  ${NEXUS_STREAMER_SRC_DIR} ${NEXUS_SRC_DIR})
target_link_libraries(nexus_streamer_ap_credentials_test PRIVATE
  nexus_identity GTest::gtest GTest::gtest_main)
set_target_properties(nexus_streamer_ap_credentials_test PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
gtest_discover_tests(nexus_streamer_ap_credentials_test)
```

- [ ] **Step 2: Build and confirm it passes (this guard is GREEN once Task 1 exists)**

Run:
```bash
cmake --build build
/usr/bin/ctest --test-dir build -R StreamerApCredentialsContract --output-on-failure
```
Expected: PASS (it depends only on Task 1's module). If it fails to link, confirm `${NEXUS_SRC_DIR}` is on the include path and `nexus_identity` is linked.

- [ ] **Step 3: Add the `--ap-credentials` CLI branch**

In `streamer/src/main/StreamerMain.cpp`, add near the top includes:
```cpp
#include "identity/ApCredentials.h"
```
Then, inside `main(int argc, char** argv)`, **before** the `--serve` handling and server construction (after `argc`/`argv` are available), add:
```cpp
  // `--ap-credentials`: print the derived private-AP SSID + passphrase for scripts/streamer-ap.sh
  // to bring up the permanent Nexus-Audio AP. Keeps the KDF in exactly one place (the C++ module).
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--ap-credentials") {
      nexus::streamer::identity::StreamerIdentity id(identityKeyPath());
      if (const auto st = id.load(); !st.ok()) {
        std::cerr << "identity load failed: " << st.message() << "\n";
        return 1;
      }
      const auto creds = nexus::identity::deriveApCredentials(id.streamerId());
      std::cout << "NEXUS_AP_SSID=" << creds.ssid << "\n"
                << "NEXUS_AP_PASSPHRASE=" << creds.passphrase << "\n";
      return 0;
    }
  }
```
(Confirm `<iostream>` and `<string>` are already included in `StreamerMain.cpp`; they are used elsewhere in the file. `identityKeyPath()` is the existing helper near the top of the file.)

- [ ] **Step 4: Build and confirm it compiles**

Run: `cmake --build build`
Expected: builds clean. (The CLI itself reads a real identity key, so it is exercised on device in Task 7, not in a host unit test.)

- [ ] **Step 5: Commit**

```bash
git add streamer/src/main/StreamerMain.cpp streamer/CMakeLists.txt streamer/tests/unit/ap_credentials_contract_test.cpp
git commit -m "feat(streamer): --ap-credentials prints derived Nexus-AP SSID/passphrase + contract test"
```

---

### Task 5: Streamer AP bring-up script (device artifact)

**Files:**
- Create: `scripts/streamer-ap.sh`

**Interfaces:**
- Consumes: `nexus-streamer --ap-credentials` (Task 4), NetworkManager/`nmcli`.
- Produces: `streamer-ap.sh up|down` — a permanent WPA2 `method=shared` AP named `Nexus-<streamer_id>` on `wlan0`.

- [ ] **Step 1: Write the script**

`scripts/streamer-ap.sh`:
```bash
#!/usr/bin/env bash
# streamer-ap.sh — bring the permanent private "Nexus-<streamer_id>" Wi-Fi AP up or down.
#
# The streamer hosts this AP so speakers join a clean, streamer-controlled network where mDNS
# multicast works (the venue AP drops client-to-client multicast — see docs/STREAMER-AP-DESIGN.md).
# Uses NetworkManager AP mode (method=shared → NM runs dnsmasq for DHCP+DNS), WPA2-secured, with
# NO captive portal (unlike the speaker setup hotspot). SSID + passphrase come from the binary's
# identity-derived KDF, so the derivation lives in exactly one place.
#
# Usage: streamer-ap.sh up | down   (normally driven by nexus-streamer-ap.service at boot)
set -euo pipefail
export PATH="/usr/sbin:/sbin:$PATH"

CON="nexus-streamer-ap"
IFACE="${NEXUS_AP_IFACE:-wlan0}"
BIN="${NEXUS_STREAMER_BIN:-/usr/local/bin/nexus-streamer}"

load_creds() {
  # Exports NEXUS_AP_SSID / NEXUS_AP_PASSPHRASE from the binary's derivation.
  eval "$("$BIN" --ap-credentials)"
  if [[ -z "${NEXUS_AP_SSID:-}" || -z "${NEXUS_AP_PASSPHRASE:-}" ]]; then
    echo "streamer-ap: failed to derive AP credentials from $BIN" >&2
    exit 1
  fi
}

case "${1:-}" in
  up)
    load_creds
    nmcli connection delete "$CON" >/dev/null 2>&1 || true
    # One-shot hotspot: sets AP mode, band, WPA2, and shared IPv4 (NM's own dnsmasq) correctly.
    nmcli device wifi hotspot ifname "$IFACE" con-name "$CON" \
      ssid "$NEXUS_AP_SSID" password "$NEXUS_AP_PASSPHRASE" >/dev/null
    # Persist + autoconnect so the AP returns after a reboot even without this unit re-running.
    nmcli connection modify "$CON" connection.autoconnect yes
    echo "streamer AP up: SSID='$NEXUS_AP_SSID' on $IFACE (WPA2, method=shared)"
    ;;
  down)
    nmcli connection down "$CON" >/dev/null 2>&1 || true
    nmcli connection delete "$CON" >/dev/null 2>&1 || true
    echo "streamer AP down: $IFACE released"
    ;;
  *)
    echo "usage: streamer-ap.sh up|down" >&2
    exit 1
    ;;
esac
```

- [ ] **Step 2: Host syntax check**

Run:
```bash
bash -n scripts/streamer-ap.sh && echo "syntax ok"
command -v shellcheck >/dev/null && shellcheck scripts/streamer-ap.sh || echo "shellcheck not installed — skipping"
chmod +x scripts/streamer-ap.sh
```
Expected: `syntax ok`. (Functional bring-up is on-device only, in Task 7 — do NOT run `up` on this workstation.)

- [ ] **Step 3: Commit**

```bash
git add scripts/streamer-ap.sh
git commit -m "feat(streamer): streamer-ap.sh — permanent WPA2 Nexus-AP via NetworkManager"
```

---

### Task 6: Streamer AP systemd unit + install docs (device artifact)

**Files:**
- Create: `deploy/nexus-streamer-ap.service`
- Modify: `docs/streamer.md` (add the AP install steps to the manual deploy block)

**Interfaces:**
- Consumes: `/usr/local/bin/nexus-streamer-ap.sh` (Task 5), `/usr/local/bin/nexus-streamer` (Task 4).
- Produces: a boot-enabled unit that brings the AP up. Runs as **root** (no `User=`), so `nmcli` works without an extra polkit rule (unlike the speaker hotspot, which runs under the service user).

- [ ] **Step 1: Write the unit**

`deploy/nexus-streamer-ap.service`:
```ini
# nexus-streamer-ap.service — permanent private "Nexus-<streamer_id>" Wi-Fi AP for speakers.
#
# Brings up the streamer's own WPA2 AP (via scripts/streamer-ap.sh) so speakers join a clean,
# streamer-controlled subnet where mDNS multicast works. See docs/STREAMER-AP-DESIGN.md. In wired
# mode the streamer's internet comes from eth0; wlan0 is the AP. Runs as root so nmcli needs no
# extra polkit grant.
#
#   systemctl enable --now nexus-streamer-ap

[Unit]
Description=Nexus Streamer Private AP
After=NetworkManager.service nexus-streamer.service
Wants=NetworkManager.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/nexus-streamer-ap.sh up
ExecStop=/usr/local/bin/nexus-streamer-ap.sh down
Environment=NEXUS_AP_IFACE=wlan0
Environment=NEXUS_STREAMER_BIN=/usr/local/bin/nexus-streamer
StandardOutput=journal
StandardError=journal
SyslogIdentifier=nexus-streamer-ap

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: Add install steps to `docs/streamer.md`**

In the "Deploy on the Pi" block of `docs/streamer.md` (after the existing `nexus-streamer.service` install lines), add:
```bash
# Private-AP (Phase A: speakers join Nexus-<streamer_id>). Requires wired eth0 uplink for internet.
sudo install -m0755 scripts/streamer-ap.sh /usr/local/bin/nexus-streamer-ap.sh
sudo install -m0644 deploy/nexus-streamer-ap.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nexus-streamer-ap.service     # brings up Nexus-<streamer_id> on wlan0
```
Also add a one-line note under it: *"Bringing up the AP takes `wlan0`; the streamer's internet must come from `eth0` (wired). The no-cable AP+STA 'juggle' fallback is Phase B (spike-gated) — not installed here."*

- [ ] **Step 3: Host check + commit**

Run: `bash -n scripts/streamer-ap.sh && echo ok` (no unit test — device-verified in Task 7).
```bash
git add deploy/nexus-streamer-ap.service docs/streamer.md
git commit -m "feat(streamer): nexus-streamer-ap.service unit + install docs for the private AP"
```

---

### Task 7: On-wire proof on the bench (GATED — requires explicit human approval)

> **STOP.** Do not run any of this without the user's explicit go-ahead. These are client devices. This task deploys to the `streamer` Pi and one speaker, reconfigures WiFi, and must be run with the streamer on a **wired eth0 uplink** (bringing up the AP takes `wlan0`, so WiFi internet for the streamer is gone). Keep a management path to the test speaker (reach it on the AP subnet `10.42.0.x`, or keep that speaker on ethernet) — moving a speaker onto the AP drops its venue-WiFi SSH path (design risk R4).

**Files:** none (deploy + verification only).

- [ ] **Step 1: Get explicit approval and confirm prerequisites**
  - User confirms: proceed on the bench; `streamer` has an ethernet cable to the venue router; pick the test speaker (recommend `speaker2`, the non-critical unit).
  - Confirm all host tasks (1–6) are committed and `feat/streamer-ap` host build + tests are green.

- [ ] **Step 2: Deploy the streamer AP** (on the `streamer` Pi, per `docs/streamer.md`)
  - Rebuild `nexus-streamer` with `-DNEXUS_STUB_HAL=OFF -DNEXUS_BUILD_TESTS=OFF` and install it (this includes the `--ap-credentials` CLI).
  - Verify creds print: `nexus-streamer --ap-credentials` → shows `NEXUS_AP_SSID=Nexus-STR-...` and a passphrase.
  - Install `streamer-ap.sh` + `nexus-streamer-ap.service`; `systemctl enable --now nexus-streamer-ap`.
  - Verify: `nmcli -t -f NAME,STATE connection show --active | grep nexus-streamer-ap`; `iw dev wlan0 info` shows AP mode; the SSID is visible from a phone.
  - Verify the streamer still has internet via eth0 (`ping -c1 1.1.1.1`).

- [ ] **Step 3: Deploy the Phase A speaker firmware to the test speaker**
  - Deploy the `feat/streamer-ap` speaker build to `speaker2` (via `scripts/deploy.sh`).
  - Ensure `speaker2` is paired to this streamer (`config.json` `pairing.streamer_id` matches the streamer's id).

- [ ] **Step 4: Verify the end-to-end join + discovery**
  - Watch `speaker2` logs: on boot it enters `CONNECTING_NETWORK`, scans, finds `Nexus-STR-...`, and joins.
  - Confirm `speaker2` got an AP-subnet IP: `nmcli -g IP4.ADDRESS device show wlan0` → `10.42.0.x`.
  - On `speaker2`: `avahi-browse -rt _nexus-streamer._tcp` **now shows the streamer's service** (this is the fix — it did not cross the venue WiFi).
  - Confirm `speaker2` advances past `SEARCHING_STREAMER` to `ONLINE` (state API / logs).

- [ ] **Step 5: Record the result**
  - Update `docs/STREAMER-AP-DESIGN.md` (and `DISCOVERY-FIX-PLAN.md` Phase 3) with the on-wire outcome.
  - Commit the doc update. Leave `speaker1` untouched until the user approves a wider rollout.
  - Report to the user: what worked, `speaker2`'s new access path, and the decision point for rolling out to `speaker1` and for Phase B (the juggle spike).

---

## Self-review

- **Spec coverage:** topology/wired-AP (Tasks 5–7), identity-derived SSID+passphrase (Task 1), speaker derive-and-join (Tasks 2–3), streamer AP hosting (Tasks 4–6), discovery contract unchanged (no task touches it — verified), host unit tests for KDF/derivation (Tasks 1, 2, 4), device steps gated (Task 7). Phase B juggle fallback correctly **excluded** (spec §5/§6). Residual-security and R4 ops-access flagged in Task 7's guard.
- **Placeholder scan:** none — every code and CMake block is complete and concrete.
- **Type consistency:** `deriveApCredentials`/`ApCredentials`/`streamerIdFromApSsid` (Task 1) are used with identical signatures in Tasks 2–4; `joinStreamerAp(NetworkManager&, const std::string&) -> core::Status` (Task 2) is called identically in Task 3 and the Application wiring; `ErrorCode::{InvalidArg,NotFound}` and `Status::{success,error,ok,code,message}` match `src/core/Result.h`/`ErrorCodes.h`.
