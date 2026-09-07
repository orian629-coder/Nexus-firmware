# Zero-Touch Speaker Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a factory-fresh/unpaired speaker power on, auto-join the streamer's Wi-Fi AP, and get auto-paired while an operator-opened provisioning window is active — no per-device config, no factory-burned pairing.

**Architecture:** Two wiring efforts plus one security gate. (1) The speaker's boot-join script gains an *unpaired* branch: scan for the strongest `Nexus-STR-…` AP, derive its credentials from the SSID via a new `nexus-speaker --ap-credentials-for-ssid` subcommand, and join. (2) The streamer gains a bearer-gated provisioning-window (default closed, auto-timeout) that drives a worker: while open, browse `_nexus-speaker._tcp`, reconstruct each new speaker's deterministic setup code from its public `device_id`, and run the existing pairing handshake. (3) The pairing boundary gains `streamer_id` shape-validation (F-E). The trust boundary is the window, because on the AP subnet the setup code is effectively public.

**Tech Stack:** C++17, CMake (presets `host-debug` = stub HAL + tests ON), GoogleTest + ctest, nlohmann::json, NetworkManager/`nmcli` + bash, Avahi (Pi-only, `NEXUS_STREAMER_AVAHI`).

**Spec:** `docs/ZERO-TOUCH-PROVISIONING-DESIGN.md`

## Global Constraints

- **Branch:** all work on `feat/streamer-ap`. Small, reviewed, frequent commits. Never commit to `master`.
- **Discovery wire contract is FROZEN** (`CLAUDE.md`): `_nexus-streamer._tcp`:8090, `_nexus-speaker._tcp`:45455, control TCP 45455, audio UDP 50005, and all TXT keys. This feature adds NO new wire fields and changes NO existing one. If you think you need to, stop and escalate.
- **Identity generation/storage is off-limits.** Do not touch identity provisioning, key material, or the `device_id`/`streamer_id` derivation. This plan only *reads and validates* them.
- **Host build must be green before any deploy:** the `build/` dir is already configured (stub HAL, tests ON); build with `cmake --build build` and run `/usr/bin/ctest --test-dir build`. Do NOT run the full-Application end-to-end tests locally (they raise polkit GUI password dialogs) — run the specific unit tests each task names.
- **ctest filter naming:** tests register via `gtest_discover_tests` under their **GTest suite name** (e.g. `SpeakerApCredentials.*`, `Pairing.*`, `WebServer.*`), and `ctest -R` is a **case-sensitive** regex over those names — NOT the target/file name. If a `-R` run prints `No tests were found!!!` it ran **nothing** and still exits 0 — that is a filter mismatch, never a pass. Always confirm a non-zero test count. Use `/usr/bin/ctest --test-dir build -N | grep <Suite>` to find the exact name.
- **Streamer libraries are per-module** (`nexus_streamer_identity`, `nexus_streamer_discovery`, `nexus_streamer_pairing`, `nexus_streamer_control`, `nexus_streamer_app`, …) — there is NO single streamer "core" lib. New streamer sources go into a new/appropriate per-module `add_library`, and test/app targets link the specific libs they use, following the existing pattern in `streamer/CMakeLists.txt`.
- **No device deploy/flash.** These are client units; the on-device bench is a separately gated human-approved step, not part of this plan's execution.
- **Fail-closed on peer-supplied data.** Any `streamer_id`/SSID coming from the wire or a scan is validated against `^STR-[0-9a-f]{8}$` / `^Nexus-STR-[0-9a-f]{8}$` before use. No `eval`, no unquoted shell interpolation.
- **Security posture:** the provisioning window is the sole real guard. Keep the setup-code reconstruction isolated in one named function so a future rotating-code scheme can replace it without touching the window or worker.

---

### Task 1: F-E — shared `streamer_id` validator + enforce at the pairing boundary

Adds a reusable shape validator to the identity module and enforces it in `PairingValidator` (currently only a non-empty check), closing the systemic gap where a malformed peer `streamer_id` is persisted and later feeds the speaker's AP-SSID KDF.

**Files:**
- Modify: `src/identity/ApCredentials.h` (declare `isWellFormedStreamerId`)
- Modify: `src/identity/ApCredentials.cpp` (implement it)
- Modify: `src/pairing/PairingValidator.cpp:13-15` (enforce it)
- Modify (DRY): `src/main/ApCredentialsCli.cpp` (replace its private `isWellFormedStreamerId` with the shared one)
- Test: `tests/unit/pairing_test.cpp` (new cases), `tests/unit/ap_credentials_test.cpp` (validator cases)

**Interfaces:**
- Produces: `bool nexus::identity::isWellFormedStreamerId(const std::string& id);` — true iff `id` matches `^STR-[0-9a-f]{8}$` (literal `STR-` + exactly 8 lowercase hex).
- Consumes: nothing from other tasks.

- [ ] **Step 1: Write the failing validator unit test**

In `tests/unit/ap_credentials_test.cpp` add:

```cpp
TEST(ApCredentials, IsWellFormedStreamerId) {
  using nexus::identity::isWellFormedStreamerId;
  EXPECT_TRUE(isWellFormedStreamerId("STR-a14ad83e"));
  EXPECT_FALSE(isWellFormedStreamerId(""));
  EXPECT_FALSE(isWellFormedStreamerId("STR-A14AD83E"));      // uppercase
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad83"));       // 7 hex
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad83ee"));     // 9 hex
  EXPECT_FALSE(isWellFormedStreamerId("SPK-a14ad83e"));      // wrong prefix
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad8;e"));      // metachar
  EXPECT_FALSE(isWellFormedStreamerId("STR-a14ad83e\n$(x)")); // injection
}
```

- [ ] **Step 2: Run it, verify it fails to compile/link**

Run: `/usr/bin/ctest --test-dir build -R ApCredentials -V`
Expected: FAIL — `isWellFormedStreamerId` not declared in `nexus::identity`.

- [ ] **Step 3: Declare and implement the shared validator**

In `src/identity/ApCredentials.h`, in `namespace nexus::identity`, add:

```cpp
// True iff id is exactly "STR-" followed by 8 lowercase hex chars.
bool isWellFormedStreamerId(const std::string& id);
```

In `src/identity/ApCredentials.cpp`:

```cpp
bool isWellFormedStreamerId(const std::string& id) {
  if (id.size() != 12) return false;
  if (id.compare(0, 4, "STR-") != 0) return false;
  for (std::size_t i = 4; i < id.size(); ++i) {
    const char c = id[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}
```

- [ ] **Step 4: Run it, verify PASS**

Run: `/usr/bin/ctest --test-dir build -R ApCredentials -V`
Expected: PASS.

- [ ] **Step 5: Write the failing PairingValidator test**

In `tests/unit/pairing_test.cpp`, add a case that builds a `PairingRequest` which passes today's checks (valid setup code, signature) but carries a malformed `streamer_id`, and asserts `validate(...)` now rejects it. Mirror the existing valid-request fixture in that file (reuse its key/signature setup); change only `req.streamer_id` to `"STR-BADID"` (or `"STR-a14ad83;"`), and assert the returned `core::Status` is an error whose message mentions `streamer_id`. Add a companion case asserting a well-formed `streamer_id` still passes (guard against over-rejection).

- [ ] **Step 6: Run it, verify it fails**

Run: `/usr/bin/ctest --test-dir build -R Pairing -V` (GTest suite `Pairing.*`)
Expected: FAIL — malformed id currently accepted.

- [ ] **Step 7: Enforce the validator in PairingValidator**

In `src/pairing/PairingValidator.cpp`, add `#include "identity/ApCredentials.h"`, and immediately after the existing non-empty check (line ~13-15):

```cpp
if (!nexus::identity::isWellFormedStreamerId(req.streamer_id))
  return InvalidArg("malformed streamer_id");
```

(Use the same `InvalidArg`/`core::Status` helper already used two lines above.)

- [ ] **Step 8: DRY — point ApCredentialsCli at the shared helper**

In `src/main/ApCredentialsCli.cpp`, delete the private `isWellFormedStreamerId` (lines ~20-29), add `#include "identity/ApCredentials.h"` if not present, and change its call site to `nexus::identity::isWellFormedStreamerId(pairing.streamer_id)`. Confirm `speaker_ap_credentials_test` still passes unchanged.

- [ ] **Step 9: Run the full affected suite, verify PASS**

Run: `/usr/bin/ctest --test-dir build -R "ApCredentials|Pairing|SpeakerApCredentials" -V`
Expected: all PASS (confirm the count is non-zero, not "No tests were found").

- [ ] **Step 10: Commit**

```bash
git add src/identity/ApCredentials.h src/identity/ApCredentials.cpp \
        src/pairing/PairingValidator.cpp src/main/ApCredentialsCli.cpp \
        tests/unit/ap_credentials_test.cpp tests/unit/pairing_test.cpp
git commit -m "feat(pairing): validate streamer_id shape at pairing boundary (F-E)"
```

---

### Task 2: Speaker CLI `nexus-speaker --ap-credentials-for-ssid <ssid>`

Adds the first production caller of `streamerIdFromApSsid`: derive AP credentials from a *scanned* SSID (for an unpaired speaker that has no stored `streamer_id`), fail-closed on any malformed/foreign SSID.

**Files:**
- Modify: `src/main/ApCredentialsCli.h` (declare)
- Modify: `src/main/ApCredentialsCli.cpp` (implement)
- Modify: `src/main/main.cpp:42-68` (arg parse + dispatch + usage)
- Test: `tests/unit/speaker_ap_credentials_test.cpp` (new cases)

**Interfaces:**
- Consumes: `nexus::identity::isWellFormedStreamerId` (Task 1); `nexus::identity::streamerIdFromApSsid(const std::string&) -> std::optional<std::string>`; `nexus::identity::deriveApCredentials(const std::string&) -> ApCredentials`.
- Produces: `std::optional<nexus::identity::ApCredentials> nexus::app::apCredentialsForSsid(const std::string& ssid);` — validates `ssid` matches `^Nexus-STR-[0-9a-f]{8}$`, recovers `streamer_id`, returns derived creds; `std::nullopt` otherwise. CLI flag `--ap-credentials-for-ssid <ssid>` prints `NEXUS_AP_SSID=`/`NEXUS_AP_PASSPHRASE=` (exit 0) or errors to stderr (exit 1).

- [ ] **Step 1: Write the failing test**

In `tests/unit/speaker_ap_credentials_test.cpp` add:

```cpp
TEST(SpeakerApCredentials, ForSsidValidMatchesDerive) {
  const auto creds = nexus::app::apCredentialsForSsid("Nexus-STR-a14ad83e");
  ASSERT_TRUE(creds.has_value());
  const auto expected = nexus::identity::deriveApCredentials("STR-a14ad83e");
  EXPECT_EQ(creds->ssid, expected.ssid);
  EXPECT_EQ(creds->passphrase, expected.passphrase);
  EXPECT_EQ(creds->ssid, "Nexus-STR-a14ad83e");
}

TEST(SpeakerApCredentials, ForSsidRejectsMalformedAndForeign) {
  EXPECT_FALSE(nexus::app::apCredentialsForSsid("").has_value());
  EXPECT_FALSE(nexus::app::apCredentialsForSsid("Nexus-Setup").has_value());
  EXPECT_FALSE(nexus::app::apCredentialsForSsid("SomeCafeWiFi").has_value());
  EXPECT_FALSE(nexus::app::apCredentialsForSsid("Nexus-STR-A14AD83E").has_value()); // uppercase
  EXPECT_FALSE(nexus::app::apCredentialsForSsid("Nexus-STR-a14ad8;e").has_value()); // metachar
  EXPECT_FALSE(nexus::app::apCredentialsForSsid("Nexus-STR-a14ad83e\n$(touch /tmp/x)").has_value());
}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `/usr/bin/ctest --test-dir build -R SpeakerApCredentials -V`
Expected: FAIL — `apCredentialsForSsid` not declared.

- [ ] **Step 3: Declare and implement**

In `src/main/ApCredentialsCli.h`, in `namespace nexus::app`:

```cpp
// Derive AP creds from a scanned SSID (unpaired bootstrap). nullopt if the
// SSID is not a well-formed Nexus streamer AP.
std::optional<nexus::identity::ApCredentials> apCredentialsForSsid(const std::string& ssid);
```

In `src/main/ApCredentialsCli.cpp` (wrap in try/catch → nullopt, matching `apCredentialsFromConfig`):

```cpp
std::optional<nexus::identity::ApCredentials> apCredentialsForSsid(const std::string& ssid) {
  try {
    // Shape-gate the SSID up front (fail-closed against injection).
    static const std::string kPrefix = "Nexus-";
    const auto id = nexus::identity::streamerIdFromApSsid(ssid); // strips "Nexus-", requires "STR-..."
    if (!id || !nexus::identity::isWellFormedStreamerId(*id)) return std::nullopt;
    return nexus::identity::deriveApCredentials(*id);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
```

- [ ] **Step 4: Run it, verify PASS**

Run: `/usr/bin/ctest --test-dir build -R SpeakerApCredentials -V`
Expected: PASS.

- [ ] **Step 5: Wire the CLI flag in main.cpp**

In `src/main/main.cpp`, alongside `want_ap_credentials` (line ~42), add parsing for a flag that takes a value:

```cpp
} else if (arg == "--ap-credentials-for-ssid") {
  if (i + 1 >= argc) { std::cerr << "nexus-speaker: --ap-credentials-for-ssid requires an SSID\n"; return 2; }
  ap_credentials_for_ssid = argv[++i];
  want_ap_credentials_for_ssid = true;
}
```

Declare `bool want_ap_credentials_for_ssid = false; std::string ap_credentials_for_ssid;` near `want_ap_credentials`. Add a usage line. In the post-parse block (near line ~59, before `nexus::Application`):

```cpp
if (want_ap_credentials_for_ssid) {
  const auto creds = nexus::app::apCredentialsForSsid(ap_credentials_for_ssid);
  if (!creds) { std::cerr << "nexus-speaker: not a Nexus streamer AP SSID\n"; return 1; }
  std::cout << "NEXUS_AP_SSID=" << creds->ssid << "\n"
            << "NEXUS_AP_PASSPHRASE=" << creds->passphrase << "\n";
  return 0;
}
```

- [ ] **Step 6: Build host, verify the binary links and the suite passes**

Run: `cmake --build build --target nexus-speaker nexus_speaker_ap_credentials_test && /usr/bin/ctest --test-dir build -R SpeakerApCredentials -V`
Expected: builds; tests PASS.

- [ ] **Step 7: Commit**

```bash
git add src/main/ApCredentialsCli.h src/main/ApCredentialsCli.cpp src/main/main.cpp \
        tests/unit/speaker_ap_credentials_test.cpp
git commit -m "feat(speaker): --ap-credentials-for-ssid derives AP creds from scanned SSID"
```

---

### Task 3: Speaker boot-join — unpaired scan-and-join branch

Extends `scripts/speaker-ap-join.sh`: when `--ap-credentials` reports "not paired", scan for the strongest `Nexus-STR-…` AP, derive creds via Task 2's CLI, and join — reusing the existing validate/nmcli/retry logic. Selection is isolated for future multi-tenant strategies.

**Files:**
- Modify: `scripts/speaker-ap-join.sh`
- Create: `tests/scripts/speaker-ap-join-select.bats.sh` (self-contained bash test; no bats dependency)
- Modify: `tests/CMakeLists.txt` (register the script test via `add_test`, mirroring `script_reset_preserves_identity` at line ~28)

**Interfaces:**
- Consumes: `nexus-speaker --ap-credentials-for-ssid <ssid>` (Task 2).
- Produces: shell function `select_streamer_ap()` — reads `nmcli -t -f SSID,SIGNAL dev wifi` on stdin, prints the single strongest SSID matching `^Nexus-STR-[0-9a-f]{8}$`, or prints nothing (exit 1) if none.

- [ ] **Step 1: Write the failing selection test**

Create `tests/scripts/speaker-ap-join-select.bats.sh`:

```bash
#!/usr/bin/env bash
# Self-contained test for select_streamer_ap(): sources the function out of the
# join script and feeds it mocked `nmcli dev wifi` output on stdin.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/../../scripts/speaker-ap-join.sh"

# Extract just the function body (script guards real actions behind a main guard).
# shellcheck disable=SC1090
source <(sed -n '/^select_streamer_ap()/,/^}/p' "$SCRIPT")

fail() { echo "FAIL: $1" >&2; exit 1; }

# strongest of two Nexus APs wins
out="$(printf '%s\n' 'Nexus-STR-a14ad83e:41' 'Nexus-STR-deadbeef:88' 'HandsomeWiFi:90' | select_streamer_ap)"
[ "$out" = "Nexus-STR-deadbeef" ] || fail "strongest: got '$out'"

# foreign / malformed SSIDs are filtered out
out="$(printf '%s\n' 'Nexus-Setup:99' 'CafeWiFi:99' 'Nexus-STR-BADCAPS:99' | select_streamer_ap || true)"
[ -z "$out" ] || fail "filtering: got '$out'"

# exactly one match
out="$(printf '%s\n' 'CafeWiFi:70' 'Nexus-STR-a14ad83e:55' | select_streamer_ap)"
[ "$out" = "Nexus-STR-a14ad83e" ] || fail "single: got '$out'"

echo "OK"
```

Make it executable: `chmod +x tests/scripts/speaker-ap-join-select.bats.sh`.

- [ ] **Step 2: Run it, verify it fails**

Run: `bash tests/scripts/speaker-ap-join-select.bats.sh`
Expected: FAIL — `select_streamer_ap` not defined in the script yet.

- [ ] **Step 3: Add `select_streamer_ap()` to the join script**

In `scripts/speaker-ap-join.sh`, add near the top (after the existing helpers, before the main flow). It must be self-contained (no side effects) so the test can source it:

```bash
# Print the strongest-signal SSID matching a Nexus streamer AP, or nothing.
# Input: `nmcli -t -f SSID,SIGNAL dev wifi` style "SSID:SIGNAL" lines on stdin.
# Isolated so a future version can swap in a multi-tenant selection strategy.
select_streamer_ap() {
  awk -F: '
    $1 ~ /^Nexus-STR-[0-9a-f]{8}$/ {
      sig = $2 + 0
      if (sig > best_sig) { best_sig = sig; best = $1 }
    }
    END { if (best != "") print best }
  '
}
```

- [ ] **Step 4: Run the selection test, verify PASS**

Run: `bash tests/scripts/speaker-ap-join-select.bats.sh`
Expected: `OK`.

- [ ] **Step 5: Add the unpaired branch to the main flow**

In `scripts/speaker-ap-join.sh`, at the point where `--ap-credentials` currently fails and the script logs "not paired" and `exit 0` (line ~41), replace the bare `exit 0` with an attempt at the unpaired path:

```bash
if ! creds="$("$BIN" --config "$CONFIG" --ap-credentials 2>/dev/null)"; then
  log "not paired — trying unpaired streamer-AP bootstrap"
  nmcli -w 10 device wifi rescan ifname "$IFACE" >/dev/null 2>&1 || true
  target_ssid="$(nmcli -t -f SSID,SIGNAL dev wifi list ifname "$IFACE" 2>/dev/null | select_streamer_ap || true)"
  if [ -z "$target_ssid" ]; then log "no Nexus streamer AP in range — nothing to join"; exit 0; fi
  log "found streamer AP ${target_ssid} — deriving creds"
  if ! creds="$("$BIN" --config "$CONFIG" --ap-credentials-for-ssid "$target_ssid" 2>/dev/null)"; then
    log "could not derive creds for ${target_ssid}"; exit 0
  fi
  UNPAIRED=1   # while unpaired, do NOT fall back to last-Wi-Fi; stay on the AP
fi
```

Then, in `restore_last_wifi()` (or at each fallback call site), guard the fallback with the unpaired flag so an unpaired speaker stays trying the AP rather than reverting:

```bash
restore_last_wifi() {
  if [ "${UNPAIRED:-0}" = "1" ]; then log "unpaired — staying on AP attempt, no Wi-Fi fallback"; return 0; fi
  # ...existing restore logic unchanged...
}
```

The rest of the script (parse `NEXUS_AP_SSID`/`NEXUS_AP_PASSPHRASE`, strict regex validation, `nexus-streamer-ap` profile create/modify, retry/backoff join) is unchanged and now serves both paired and unpaired paths.

- [ ] **Step 6: Shellcheck + re-run selection test**

Run: `shellcheck scripts/speaker-ap-join.sh || true` (fix any new warnings you introduced) and `bash tests/scripts/speaker-ap-join-select.bats.sh`
Expected: no new shellcheck errors in changed lines; selection test `OK`.

- [ ] **Step 7: Register the script test with ctest**

In `tests/CMakeLists.txt`, near the existing `add_test(NAME script_reset_preserves_identity ...)` (line ~28), add:

```cmake
add_test(NAME script_speaker_ap_join_select
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/scripts/speaker-ap-join-select.bats.sh)
```

- [ ] **Step 8: Verify via ctest**

Run: `cmake --build build && /usr/bin/ctest --test-dir build -R script_speaker_ap_join_select -V`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add scripts/speaker-ap-join.sh tests/scripts/speaker-ap-join-select.bats.sh tests/CMakeLists.txt
git commit -m "feat(speaker): boot-join unpaired branch scans+joins strongest Nexus streamer AP"
```

---

### Task 4: Streamer `ProvisioningWindow` state

A small, thread-safe holder for the provisioning window: open/closed with an expiry, queried by the API and the worker. Pure logic, no I/O — fully host-testable.

**Files:**
- Create: `streamer/src/provisioning/ProvisioningWindow.h`
- Create: `streamer/src/provisioning/ProvisioningWindow.cpp`
- Create: `streamer/tests/unit/provisioning_window_test.cpp`
- Modify: `streamer/CMakeLists.txt` (new lib source + `nexus_streamer_provisioning_window_test`, mirroring `nexus_streamer_identity_test` at line ~310)

**Interfaces:**
- Produces `namespace nexus::streamer::provisioning`:
  - `class ProvisioningWindow { public: void open(std::int64_t now_epoch, int ttl_seconds); void close(); bool isOpen(std::int64_t now_epoch) const; int secondsRemaining(std::int64_t now_epoch) const; };`
  - Thread-safe (internal mutex). `open` clamps `ttl_seconds` to `[1, kMaxTtlSeconds]` (kMaxTtlSeconds = 1800). Default state closed. `isOpen` returns false once `now_epoch >= expiry`.

- [ ] **Step 1: Write the failing test**

Create `streamer/tests/unit/provisioning_window_test.cpp`:

```cpp
#include "provisioning/ProvisioningWindow.h"
#include <gtest/gtest.h>
using nexus::streamer::provisioning::ProvisioningWindow;

TEST(ProvisioningWindow, DefaultClosed) {
  ProvisioningWindow w;
  EXPECT_FALSE(w.isOpen(1000));
  EXPECT_EQ(w.secondsRemaining(1000), 0);
}
TEST(ProvisioningWindow, OpenThenExpires) {
  ProvisioningWindow w;
  w.open(/*now=*/1000, /*ttl=*/600);
  EXPECT_TRUE(w.isOpen(1000));
  EXPECT_TRUE(w.isOpen(1599));
  EXPECT_EQ(w.secondsRemaining(1000), 600);
  EXPECT_FALSE(w.isOpen(1600));            // expired at boundary
  EXPECT_EQ(w.secondsRemaining(1600), 0);
}
TEST(ProvisioningWindow, ManualClose) {
  ProvisioningWindow w;
  w.open(1000, 600);
  w.close();
  EXPECT_FALSE(w.isOpen(1000));
}
TEST(ProvisioningWindow, TtlClamped) {
  ProvisioningWindow w;
  w.open(0, 100000);                        // over max
  EXPECT_TRUE(w.isOpen(1799));
  EXPECT_FALSE(w.isOpen(1800));             // clamped to 1800
  w.open(0, -5);                            // under min
  EXPECT_TRUE(w.isOpen(0));
  EXPECT_FALSE(w.isOpen(1));
}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `/usr/bin/ctest --test-dir build -R ProvisioningWindow -V`
Expected: FAIL — header/target missing.

- [ ] **Step 3: Implement the class**

`streamer/src/provisioning/ProvisioningWindow.h`:

```cpp
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
```

`streamer/src/provisioning/ProvisioningWindow.cpp`:

```cpp
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
```

- [ ] **Step 4: Register in CMake**

In `streamer/CMakeLists.txt`, follow the **per-module library** pattern (there is no single core lib). Create a new module library for provisioning and register the test, guarded by the same `if(NEXUS_BUILD_TESTS)` block the neighbors use:

```cmake
add_library(nexus_streamer_provisioning STATIC src/provisioning/ProvisioningWindow.cpp)
target_include_directories(nexus_streamer_provisioning PUBLIC src)   # match how neighbor libs expose streamer/src
target_link_libraries(nexus_streamer_provisioning PUBLIC nexus_core) # for std types/Result if needed; mirror a neighbor lib

# ... inside the if(NEXUS_BUILD_TESTS) block, alongside nexus_streamer_identity_test:
add_executable(nexus_streamer_provisioning_window_test tests/unit/provisioning_window_test.cpp)
target_include_directories(nexus_streamer_provisioning_window_test PRIVATE src)
target_link_libraries(nexus_streamer_provisioning_window_test PRIVATE nexus_streamer_provisioning GTest::gtest GTest::gtest_main)
gtest_discover_tests(nexus_streamer_provisioning_window_test)
```

(Copy the exact `target_include_directories`/`set_target_properties` lines from the `nexus_streamer_identity_test` neighbor at streamer/CMakeLists.txt:~304-311 so include paths and C++ standard match. `AutoPairWorker.cpp` will be added to this same `nexus_streamer_provisioning` library in Task 6.)

- [ ] **Step 5: Build + run, verify PASS**

Run: `cmake --build build --target nexus_streamer_provisioning_window_test && /usr/bin/ctest --test-dir build -R ProvisioningWindow -V`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add streamer/src/provisioning/ProvisioningWindow.h streamer/src/provisioning/ProvisioningWindow.cpp \
        streamer/tests/unit/provisioning_window_test.cpp streamer/CMakeLists.txt
git commit -m "feat(streamer): ProvisioningWindow open/close/expiry state"
```

---

### Task 5: Streamer `GET/POST /api/provisioning-window` route

Exposes the window over the (already bearer-gated) API so the future web-UI toggle can drive it. Adds a `ProvisioningWindow&` to the router and two branches.

**Files:**
- Modify: `streamer/src/web/StreamerApiRouter.h` (ctor takes `provisioning::ProvisioningWindow*`; store it)
- Modify: `streamer/src/web/StreamerApiRouter.cpp` (GET branch in `handleGet`, POST branch in `handlePost`)
- Modify: `streamer/src/app/StreamerApp.cpp` (own a `ProvisioningWindow`, pass it to the router ctor)
- Test: `streamer/tests/unit/` web/router test (extend the existing `nexus_streamer_web_test` fixture)

**Interfaces:**
- Consumes: `ProvisioningWindow` (Task 4); the router's `route()` bearer-auth gate (cpp:26-32) — no extra auth code.
- Produces: `POST /api/provisioning-window {"open":bool,"ttl":int}` → `200 {"open":bool,"seconds_remaining":int}`; `GET /api/provisioning-window` → `200 {"open":bool,"seconds_remaining":int}`. Malformed body → `400`.

- [ ] **Step 1: Write the failing router test**

In the existing streamer web/router test file (the one compiled into `nexus_streamer_web_test`), add cases that construct the router with a `ProvisioningWindow` and a valid bearer token, then:
- `GET /api/provisioning-window` with a good token → 200, body `open=false`, `seconds_remaining=0`.
- `POST /api/provisioning-window {"open":true,"ttl":600}` → 200, `open=true`, `seconds_remaining` in `(0,600]`.
- subsequent `GET` → `open=true`.
- `POST {"open":false}` → 200, `open=false`.
- `POST` with no token → 401 (proves the gate covers the new route).
- `POST {"open":true}` with a non-numeric `ttl` (e.g. `"ttl":"x"`) → 400.

Model the request/response construction on the existing cases in that file (reuse its `Authentication`/token setup and its `HttpRequest` builder). Inject a fixed `now_epoch` if the fixture allows; otherwise assert `seconds_remaining > 0` rather than an exact value.

- [ ] **Step 2: Run it, verify it fails**

Run: `/usr/bin/ctest --test-dir build -R WebServer -V`
Expected: FAIL — route returns 404/unknown today.

- [ ] **Step 3: Thread ProvisioningWindow into the router**

In `StreamerApiRouter.h`, add a `provisioning::ProvisioningWindow* window_` member and a ctor parameter for it (nullable-safe: if null, the route returns `503`). Forward-declare or include `provisioning/ProvisioningWindow.h`.

- [ ] **Step 4: Implement the branches**

In `StreamerApiRouter.cpp`, in `handleGet(path, host)` add:

```cpp
if (path == "/api/provisioning-window") {
  if (!window_) return json(503, {{"error", "provisioning unavailable"}});
  const auto now = /* same clock helper the router/app uses for epoch seconds */;
  return json(200, {{"open", window_->isOpen(now)},
                    {"seconds_remaining", window_->secondsRemaining(now)}});
}
```

In `handlePost(req)` (near the `/api/pair` block, cpp:369) add:

```cpp
if (p == "/api/provisioning-window") {
  if (!window_) return json(503, {{"error", "provisioning unavailable"}});
  nlohmann::json body;
  try { body = nlohmann::json::parse(req.body); } catch (...) { return badRequest("invalid json"); }
  if (!body.contains("open") || !body["open"].is_boolean()) return badRequest("missing 'open'");
  const auto now = /* epoch-seconds helper */;
  if (body["open"].get<bool>()) {
    int ttl = 600;
    if (body.contains("ttl")) {
      if (!body["ttl"].is_number_integer()) return badRequest("ttl must be an integer");
      ttl = body["ttl"].get<int>();
    }
    window_->open(now, ttl);
  } else {
    window_->close();
  }
  return json(200, {{"open", window_->isOpen(now)},
                    {"seconds_remaining", window_->secondsRemaining(now)}});
}
```

Use the same `json(...)`/`badRequest(...)` helpers (cpp:13-18) and the same epoch-seconds source the app already uses (find how `PairingService`/router obtain `now_epoch`; reuse it — do not introduce a second clock).

- [ ] **Step 5: Construct + inject the window in StreamerApp**

In `streamer/src/app/StreamerApp.cpp`, add a `provisioning::ProvisioningWindow provisioning_window_;` member and pass `&provisioning_window_` into the `StreamerApiRouter` ctor at the construction site (near line ~201). Keep it a stable member (the worker in Task 7 shares it).

- [ ] **Step 6: Build + run, verify PASS**

Run: `cmake --build build && /usr/bin/ctest --test-dir build -R WebServer -V`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add streamer/src/web/StreamerApiRouter.h streamer/src/web/StreamerApiRouter.cpp \
        streamer/src/app/StreamerApp.cpp streamer/tests/unit/*web*
git commit -m "feat(streamer): /api/provisioning-window open/close/status route"
```

---

### Task 6: Streamer auto-pair worker

The core behavior: given a discovery source, the window, a "already-registered?" predicate, and a pair callback, sweep the subnet while open and pair each new unpaired speaker — reconstructing the deterministic setup code from the public `device_id`. Dependency-injected so it is fully host-testable with `StubStreamerDiscovery` and fakes.

**Files:**
- Create: `streamer/src/provisioning/AutoPairWorker.h`
- Create: `streamer/src/provisioning/AutoPairWorker.cpp`
- Create: `streamer/tests/unit/auto_pair_worker_test.cpp`
- Modify: `streamer/CMakeLists.txt` (source + `nexus_streamer_auto_pair_worker_test`)

**Interfaces:**
- Consumes: `ProvisioningWindow` (Task 4); `nexus::streamer::discovery::IStreamerDiscovery` + `DiscoveredSpeaker` (`streamer/src/discovery/IStreamerDiscovery.h`).
- Produces `namespace nexus::streamer::provisioning`:
  - `std::string deriveSetupCode(const std::string& device_id);` — returns `"SETUP-" + device_id.substr(4)`; empty string if `device_id` is not `SPK-` + ≥1 char. **This is the isolated, replaceable trust-anchor function.**
  - `struct PairAttempt { std::string device_id; std::string host; std::string box_public_key; std::string setup_code; };`
  - `using PairFn = std::function<bool(const PairAttempt&)>;` (returns true on success)
  - `using IsRegisteredFn = std::function<bool(const std::string& device_id)>;`
  - `class AutoPairWorker { public: AutoPairWorker(discovery::IStreamerDiscovery&, ProvisioningWindow&, IsRegisteredFn, PairFn, std::function<std::int64_t()> now); int sweepOnce(); };` — `sweepOnce()` returns the count of speakers paired this sweep; a no-op returning 0 when the window is closed. (A background thread that calls `sweepOnce()` on an interval is wired in Task 7; the class stays synchronously testable.)

- [ ] **Step 1: Write the failing test**

Create `streamer/tests/unit/auto_pair_worker_test.cpp`:

```cpp
#include "provisioning/AutoPairWorker.h"
#include "provisioning/ProvisioningWindow.h"
#include "discovery/IStreamerDiscovery.h"
#include <gtest/gtest.h>
#include <set>
using namespace nexus::streamer;
using nexus::streamer::provisioning::AutoPairWorker;
using nexus::streamer::provisioning::ProvisioningWindow;
using nexus::streamer::provisioning::PairAttempt;
using nexus::streamer::discovery::DiscoveredSpeaker;

namespace {
struct FakeDiscovery : nexus::streamer::discovery::IStreamerDiscovery {
  std::vector<DiscoveredSpeaker> speakers;
  nexus::core::Result<std::vector<DiscoveredSpeaker>> browseSpeakers() override { return speakers; }
  // ...stub any other pure-virtuals minimally (advertise/etc.) to return ok...
};
DiscoveredSpeaker spk(const std::string& id) {
  DiscoveredSpeaker d; d.device_id = id; d.host = id + ".local";
  d.box_public_key = "BOXKEY"; d.control_port = 45455; d.setup_mode = true; return d;
}
}  // namespace

TEST(DeriveSetupCode, MatchesSpeakerRule) {
  EXPECT_EQ(provisioning::deriveSetupCode("SPK-a1b2c3d4"), "SETUP-a1b2c3d4");
  EXPECT_EQ(provisioning::deriveSetupCode("bad"), "");
}

TEST(AutoPairWorker, PairsNewSpeakerWhenOpen) {
  FakeDiscovery disc; disc.speakers = { spk("SPK-a1b2c3d4") };
  ProvisioningWindow win; win.open(1000, 600);
  std::set<std::string> registered;
  std::vector<PairAttempt> attempts;
  AutoPairWorker w(disc, win,
    /*isRegistered=*/[&](const std::string& id){ return registered.count(id) > 0; },
    /*pair=*/[&](const PairAttempt& a){ attempts.push_back(a); registered.insert(a.device_id); return true; },
    /*now=*/[]{ return (std::int64_t)1000; });
  EXPECT_EQ(w.sweepOnce(), 1);
  ASSERT_EQ(attempts.size(), 1u);
  EXPECT_EQ(attempts[0].device_id, "SPK-a1b2c3d4");
  EXPECT_EQ(attempts[0].setup_code, "SETUP-a1b2c3d4");
  EXPECT_EQ(attempts[0].box_public_key, "BOXKEY");
  EXPECT_EQ(w.sweepOnce(), 0);   // already registered → not re-paired
}

TEST(AutoPairWorker, NoOpWhenClosed) {
  FakeDiscovery disc; disc.speakers = { spk("SPK-a1b2c3d4") };
  ProvisioningWindow win;  // closed
  AutoPairWorker w(disc, win, [](auto&){return false;}, [](auto&){return true;}, []{return (std::int64_t)1000;});
  EXPECT_EQ(w.sweepOnce(), 0);
}

TEST(AutoPairWorker, SkipsNonSetupModeAndBadIds) {
  FakeDiscovery disc;
  auto paired = spk("SPK-deadbeef"); paired.setup_mode = false;      // not in setup mode
  auto badid = spk("ROGUE"); // malformed device_id → no setup code
  disc.speakers = { paired, badid };
  ProvisioningWindow win; win.open(1000, 600);
  int calls = 0;
  AutoPairWorker w(disc, win, [](auto&){return false;},
                   [&](auto&){ ++calls; return true; }, []{return (std::int64_t)1000;});
  EXPECT_EQ(w.sweepOnce(), 0);
  EXPECT_EQ(calls, 0);
}
```

- [ ] **Step 2: Run it, verify it fails**

Run: `/usr/bin/ctest --test-dir build -R "AutoPairWorker|DeriveSetupCode" -V`
Expected: FAIL — header/target missing.

- [ ] **Step 3: Implement `AutoPairWorker`**

`streamer/src/provisioning/AutoPairWorker.h`:

```cpp
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "discovery/IStreamerDiscovery.h"
#include "provisioning/ProvisioningWindow.h"
namespace nexus::streamer::provisioning {

std::string deriveSetupCode(const std::string& device_id);  // isolated trust anchor

struct PairAttempt { std::string device_id, host, box_public_key, setup_code; };
using PairFn = std::function<bool(const PairAttempt&)>;
using IsRegisteredFn = std::function<bool(const std::string&)>;

class AutoPairWorker {
 public:
  AutoPairWorker(discovery::IStreamerDiscovery& disc, ProvisioningWindow& win,
                 IsRegisteredFn is_registered, PairFn pair, std::function<std::int64_t()> now);
  int sweepOnce();
 private:
  discovery::IStreamerDiscovery& disc_;
  ProvisioningWindow& win_;
  IsRegisteredFn is_registered_;
  PairFn pair_;
  std::function<std::int64_t()> now_;
};
}  // namespace nexus::streamer::provisioning
```

`streamer/src/provisioning/AutoPairWorker.cpp`:

```cpp
#include "provisioning/AutoPairWorker.h"
namespace nexus::streamer::provisioning {

std::string deriveSetupCode(const std::string& device_id) {
  if (device_id.size() <= 4 || device_id.compare(0, 4, "SPK-") != 0) return "";
  return "SETUP-" + device_id.substr(4);
}

AutoPairWorker::AutoPairWorker(discovery::IStreamerDiscovery& disc, ProvisioningWindow& win,
                               IsRegisteredFn is_registered, PairFn pair, std::function<std::int64_t()> now)
    : disc_(disc), win_(win), is_registered_(std::move(is_registered)),
      pair_(std::move(pair)), now_(std::move(now)) {}

int AutoPairWorker::sweepOnce() {
  const auto now = now_();
  if (!win_.isOpen(now)) return 0;
  auto result = disc_.browseSpeakers();
  if (!result) return 0;                        // browse failure → nothing this sweep
  int paired = 0;
  for (const auto& s : result.value()) {
    if (!s.setup_mode) continue;
    if (is_registered_(s.device_id)) continue;
    const std::string code = deriveSetupCode(s.device_id);
    if (code.empty()) continue;                 // malformed device_id → skip
    if (!win_.isOpen(now_())) break;            // window may have closed mid-sweep
    PairAttempt a{s.device_id, s.host, s.box_public_key, code};
    if (pair_(a)) ++paired;
  }
  return paired;
}
}  // namespace nexus::streamer::provisioning
```

(Adjust `result`/`.value()`/`if (!result)` to the actual `core::Result<T>` API used elsewhere in the streamer tree — copy the access pattern from an existing `browseSpeakers()`/`Result` consumer.)

- [ ] **Step 4: Register in CMake**

In `streamer/CMakeLists.txt`: add `src/provisioning/AutoPairWorker.cpp` to the **`nexus_streamer_provisioning`** library created in Task 4, and make that library link the discovery module (it now consumes `IStreamerDiscovery`/`DiscoveredSpeaker`):

```cmake
# extend the existing add_library from Task 4:
add_library(nexus_streamer_provisioning STATIC src/provisioning/ProvisioningWindow.cpp src/provisioning/AutoPairWorker.cpp)
target_link_libraries(nexus_streamer_provisioning PUBLIC nexus_streamer_discovery nexus_core)

# inside if(NEXUS_BUILD_TESTS):
add_executable(nexus_streamer_auto_pair_worker_test tests/unit/auto_pair_worker_test.cpp)
target_include_directories(nexus_streamer_auto_pair_worker_test PRIVATE src)
target_link_libraries(nexus_streamer_auto_pair_worker_test PRIVATE nexus_streamer_provisioning nexus_streamer_discovery GTest::gtest GTest::gtest_main)
gtest_discover_tests(nexus_streamer_auto_pair_worker_test)
```

(Confirm the discovery module's real target name from the `nexus_streamer_discovery_test` neighbor at streamer/CMakeLists.txt:~325-329 — it links `nexus_streamer_discovery nexus_core`.)

- [ ] **Step 5: Build + run, verify PASS**

Run: `cmake --build build --target nexus_streamer_auto_pair_worker_test && /usr/bin/ctest --test-dir build -R "AutoPairWorker|DeriveSetupCode" -V`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add streamer/src/provisioning/AutoPairWorker.h streamer/src/provisioning/AutoPairWorker.cpp \
        streamer/tests/unit/auto_pair_worker_test.cpp streamer/CMakeLists.txt
git commit -m "feat(streamer): auto-pair worker sweeps subnet and pairs new speakers when window open"
```

---

### Task 7: Wire the auto-pair worker into StreamerApp

Connects the worker to the real discovery impl, the real `PairingClient` handshake, and the registry, and runs `sweepOnce()` on a background interval that only does work while the window is open. This is the integration task — no new behavior, just binding, and it must not regress startup.

**Files:**
- Modify: `streamer/src/app/StreamerApp.cpp` (construct discovery, build the `PairFn` around `PairingClient`, build the `IsRegisteredFn` over the registry, start/stop a sweep thread)
- Modify: `streamer/CMakeLists.txt` — make the `nexus_streamer_app` library (and the `nexus_streamer_app_test` target) link `nexus_streamer_provisioning`; add other module deps (`nexus_streamer_pairing`, `nexus_streamer_discovery`) only if not already linked.
- Test: extend `nexus_streamer_app_test` (suite likely `AppLifecycle.*` / as named in `tests/unit/app_lifecycle_test.cpp`; confirm via `-N`) with a construction/smoke assertion (no real network)

**Interfaces:**
- Consumes: `AutoPairWorker`, `deriveSetupCode`, `PairAttempt` (Task 6); `ProvisioningWindow` member added in Task 5; `PairingClient::pair(PairingParams)` and the identity-injection pattern at `StreamerApp.cpp:173-179`; `browseSpeakers()` impl (`AvahiStreamerDiscovery` under `NEXUS_STREAMER_AVAHI`, else `StubStreamerDiscovery`); the registry + `persist_()` used by `/api/pair` success.
- Produces: a running streamer that auto-pairs discovered speakers while its window is open. No new public interface.

- [ ] **Step 1: Build the `PairFn` adapter**

In `StreamerApp.cpp`, write a lambda capturing the streamer identity and transport that converts a `PairAttempt` into a `PairingParams` (fill `streamer_id`/keys from `id_` exactly as the existing `PairingSender` at 173-179 does; `speaker_box_public_key = a.box_public_key`; `setup_code = a.setup_code`; `host = a.host`; `control_port = 45455`), calls `PairingClient(line_, a.host, 45455).pair(params)`, and on `ok` performs the SAME registry upsert + `persist_()` the `/api/pair` handler does, plus an audit log line (`device_id`, timestamp). Returns `reply.ok`. Reuse existing helpers — do not duplicate the pairing/persist logic; if it lives inline in the router, extract a shared helper both call, or factor the minimal shared step.

- [ ] **Step 2: Build the `IsRegisteredFn`**

A lambda that checks the streamer registry for `device_id` (same registry the router queries). Guard with the registry's mutex if it has one.

- [ ] **Step 3: Construct discovery + worker**

Instantiate the discovery implementation the build selects (real `AvahiStreamerDiscovery` when `NEXUS_STREAMER_AVAHI`, else `StubStreamerDiscovery`) — reuse whatever the app already builds for browsing if present; otherwise construct it here. Construct `AutoPairWorker(disc, provisioning_window_, isRegistered, pairFn, nowFn)` where `nowFn` is the app's existing epoch-seconds source.

- [ ] **Step 4: Run sweeps on a background interval**

Add a worker thread (follow the app's existing threaded-service pattern, e.g. the advertise/telemetry threads) that loops: sleep a short interval (e.g. 3 s), call `worker.sweepOnce()`, exit cleanly on shutdown. The thread is cheap when closed (`sweepOnce` returns 0 immediately). Ensure it is joined in the app's stop/teardown path. Do NOT block startup on it.

- [ ] **Step 5: Extend the app smoke test**

In the file behind `nexus_streamer_app_test`, add/extend a test that constructs `StreamerApp` (as existing app tests do) and asserts it builds and tears down cleanly with the provisioning window closed (no speakers paired, no crash). If the existing fixture cannot inject discovery, keep this a construction/shutdown smoke assertion only — the behavioral coverage lives in Task 6.

- [ ] **Step 6: Full host build + full suite**

Run: `cmake --build build && /usr/bin/ctest --test-dir build -E "e2e|end_to_end|scenario|hardware"`
Expected: all selected tests PASS (excluding the polkit-triggering full-Application e2e suites per Global Constraints).

- [ ] **Step 7: Commit**

```bash
git add streamer/src/app/StreamerApp.cpp streamer/CMakeLists.txt streamer/tests/unit/*app*
git commit -m "feat(streamer): wire auto-pair worker into app (window-gated background sweep)"
```

---

## Post-plan verification (controller, after Task 7)

- Full host build green; `/usr/bin/ctest` green excluding the polkit e2e suites.
- Grep the tree: `streamerIdFromApSsid` now has a production caller; `deriveSetupCode` is the only place the setup code is reconstructed.
- Confirm NO wire-contract constant or TXT key changed (`git diff feat/streamer-ap~7 -- '*protocol*' 'src/discovery' 'streamer/src/discovery'` shows no field additions).
- The on-device bench ("clear a speaker's pairing → power on → auto-join → auto-pair", window opened via API, behind a recover-if-it-fails safety net) is a **separately human-gated** step — NOT part of this plan's execution.
