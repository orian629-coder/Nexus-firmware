# Zero-Touch Speaker Provisioning — Design

**Author:** Ariel (consulting) · **Branch:** `feat/streamer-ap` · **Status:** DRAFT for review (no code yet).
**Depends on:** the streamer-AP feature (`docs/STREAMER-AP-DESIGN.md`) and the boot-join work
(`scripts/speaker-ap-join.sh`, `deploy/nexus-speaker-ap-join.service`), both already on device.

## Goal

A factory-fresh / unpaired speaker should provision itself with **no per-device configuration and
no factory-burned pairing**:

> **power on → auto-join the streamer's AP → get auto-paired**, while the operator has a
> provisioning window open on the streamer.

Today this is impossible: an unpaired speaker cannot derive the AP SSID (it has no `streamer_id`),
and pairing is operator-initiated with a hand-entered setup code. This design closes both gaps.

## What already works (do not rebuild)

- **Paired** speakers derive their streamer's AP creds and auto-join at boot
  (`nexus-speaker --ap-credentials` → `scripts/speaker-ap-join.sh`). Proven on device incl. a
  full-Wi-Fi-wipe "first-time" join.
- The streamer runs a permanent WPA2 AP `Nexus-<streamer_id>` on the clean `10.42.0.0/24` subnet.
- The pairing **handshake** exists end to end: `PairingClient::pair(PairingParams)` on the streamer
  (`streamer/src/pairing/PairingClient.h`) ↔ `PairingService::handlePairing` +
  `PairingValidator::validate` on the speaker (`src/pairing/`).
- The streamer can **enumerate** unpaired speakers: `IStreamerDiscovery::browseSpeakers()` →
  `std::vector<DiscoveredSpeaker>{device_id, host, box_public_key, control_port, setup_mode}`
  (`streamer/src/discovery/IStreamerDiscovery.h`). An unpaired speaker **advertises**
  `_nexus-speaker._tcp` in setup mode once it boots (`src/main/Application.cpp:608-631`).

The pieces exist; they are **unwired**. The streamer never browses (`Discoverer` is `nullptr`,
`StreamerApp.cpp:180`), and `streamerIdFromApSsid()` (`src/identity/ApCredentials.h`) has **zero
callers**. This design is mostly wiring plus one security gate.

---

## Trust model (read this first — it drives the whole design)

The pairing handshake has three factors; only one is a real barrier on the AP subnet:

| Factor | Reality on the AP subnet |
|---|---|
| AP passphrase | Derived from `streamer_id` (insider-derivable) — **not** a strong boundary. |
| `setup_code` | A POC placeholder: `"SETUP-" + device_id.substr(4)` (`Application.cpp:617`). `device_id` is broadcast in the clear (beacon TXT + unauth `/api/status`), so it is **effectively public**. Adds no real barrier. |
| X25519 `box_public_key` possession | The speaker proving it owns the secret for its advertised `box_public_key` is the one genuine crypto factor, plus the streamer's Ed25519 signature. |

**Conclusion:** because everything the handshake checks is either public or insider-derivable on
the subnet, the **provisioning window on the streamer is the actual trust boundary.** That is what
we gate on. This is a deliberate, documented POC posture — see *Forward-compatibility* for what
changes when a production rotating setup code lands.

### The provisioning window (the guard)

- Streamer holds a **window state**: `closed` by default, `open` with an **expiry timestamp**.
- Opening/closing is an authenticated action: `POST /api/provisioning-window`. All `/api/` routes
  are already bearer-gated (`StreamerApiRouter::route`, cpp:26-32) — no extra auth work.
- **Stays open**, pairing every new speaker it sees, until the operator toggles it off **or** a
  **safety timeout (default 600 s)** fires. Matches the intended web-UI ON/OFF toggle
  ("locate new speakers" → UI prompts the operator to power on the new speaker).
- The streamer **only auto-pairs while the window is open**, never a speaker already in its
  registry, and **audit-logs** every auto-pair (device_id, time, source).

---

## Component 1 — Speaker: unpaired AP bootstrap (the "R2" gap)

Extends the existing boot-join. `scripts/speaker-ap-join.sh` today calls `--ap-credentials`, which
fail-closes when unpaired (dead end). Add an **unpaired branch** taken only when that returns
"not paired":

1. **Scan** `wlan0` for SSIDs matching `^Nexus-STR-[0-9a-f]{8}$` (`nmcli -f SSID,SIGNAL dev wifi`).
2. **Select** the **strongest-signal** match. Selection is isolated behind a single shell function
   `select_streamer_ap()` so a future version can swap in a smarter strategy (multi-tenant
   disambiguation) **without factory-burned IDs** — see *Forward-compatibility*.
3. **Derive** creds for that SSID via a **new CLI subcommand**
   `nexus-speaker --ap-credentials-for-ssid <ssid>` (below), then join with the exact same
   no-`eval` + strict-regex + retry/backoff discipline the paired path already uses.
4. **While unpaired, stay on the AP** (do not fall back to a nonexistent last-Wi-Fi). Once joined,
   `nexus-speaker` boots into `SetupMode` and advertises `_nexus-speaker._tcp` on the subnet,
   where the streamer's open window can find it.

### New CLI: `nexus-speaker --ap-credentials-for-ssid <ssid>`

- New `nexus::app::apCredentialsForSsid(const std::string& ssid)` in
  `src/main/ApCredentialsCli.{h,cpp}`, dispatched in `main.cpp` like `--ap-credentials`.
- Re-validates the SSID shape **fail-closed** (same posture as `isWellFormedStreamerId`), calls
  `nexus::identity::streamerIdFromApSsid(ssid)` (wiring its first production caller) → on success
  `deriveApCredentials(streamer_id)`; prints the same `NEXUS_AP_SSID=` / `NEXUS_AP_PASSPHRASE=`
  format. Any malformed/foreign SSID → exit 1, no output. No device secrets are read; this is pure
  derivation from a scanned public SSID.

---

## Component 2 — Streamer: provisioning window + auto-pair

### 2a. Window state + API

- New small `ProvisioningWindow` holder (open flag + expiry epoch), owned by `StreamerApp`,
  thread-safe (a browse worker reads it).
- New route branch in `StreamerApiRouter::handlePost` (near the `/api/pair` block, cpp:369):
  - `POST /api/provisioning-window {"open": true, "ttl": 600}` → open with expiry `now+ttl`
    (ttl clamped to a max, e.g. 1800 s).
  - `POST /api/provisioning-window {"open": false}` → close.
  - `GET /api/provisioning-window` → `{open, seconds_remaining}` for the future web-UI toggle.
  - Idempotent; already bearer-gated by `route()`.

### 2b. Auto-pair worker

A bounded worker (reuses the browse machinery already present) that runs **only while the window is
open**:

1. `browseSpeakers()` → `DiscoveredSpeaker` list on the subnet.
2. For each speaker **not already in the registry** and `setup_mode == true`:
   - Reconstruct the setup code from the discovered public `device_id`:
     `"SETUP-" + device_id.substr(4)` (the exact rule the speaker checks,
     `Application.cpp:617` / `PairingValidator.cpp:24`).
   - Build `PairingParams` — streamer identity injected exactly as the existing `PairingSender`
     does (`StreamerApp.cpp:173-179`), `speaker_box_public_key` from the discovered record, `host`
     from the resolved subnet host — and call `PairingClient::pair(params)`.
   - On `ok`: registry upsert + `persist_()` (same as `/api/pair` success), audit-log the pairing.
   - On failure: log and continue; the window stays open and retries on the next sweep.
3. Sweep on a modest interval; stop promptly when the window closes or expires.

This deliberately reuses the operator `/api/pair` success path — auto-pair is that path with the
setup code derived instead of typed, fired from inside an open, authenticated window.

---

## Component 3 — F-E: validate `streamer_id` / peer ids at the pairing boundary

Fold in the previously-flagged F-E fix, since auto-pair makes this path unattended:

- In `PairingValidator::validate` (`src/pairing/PairingValidator.cpp:13-15`, currently only a
  non-empty check), add a **shape** check: `streamer_id` must match `^STR-[0-9a-f]{8}$`.
- Audit the adjacent peer-supplied fields (`streamer_public_key` base64 shape) similarly.
- This protects **every** consumer of the stored `streamer_id` (it feeds the speaker's AP-SSID
  KDF), not just this feature. Consumer side (`--ap-credentials`, the join script) already
  validates; this closes the systemic producer gap.

---

## End-to-end data flow (the demo)

1. Operator opens the window (future: web-UI toggle → `POST /api/provisioning-window`). UI shows
   "power on the new speaker."
2. Unpaired speaker powers on → boot-join takes the unpaired branch → scans → strongest
   `Nexus-STR-…` → derives creds → joins the AP → lands on `10.42.0.x`.
3. `nexus-speaker` boots `Unconfigured → SetupMode`, advertises `_nexus-speaker._tcp` on the subnet.
4. Streamer's open-window worker browses, sees the new speaker, derives its setup code, runs the
   Ed25519/X25519 handshake, **pairs and persists** on both sides. Audit-logged.
5. The speaker is now a normal paired unit: on every later boot it derives its own AP creds from
   the persisted pairing and rejoins — identical to speaker1/speaker2 today.
6. Window stays open for additional speakers until the operator toggles off or the timeout fires.

---

## Out of scope (explicit)

- **F-B (`AUTHENTICATING → ONLINE`)** is *not* fixed here. Pairing completes **before** the online
  flow, so "boot → auto-join → auto-pair" is fully demonstrable without it — but the bench will show
  a **paired** speaker, not a **playing** one. F-B remains a separate track.
- Audio, DSP, and the streamer-AP juggle spike (Phase B) are untouched.

## Forward-compatibility (write for these; don't build them now)

- **Rotating production setup code.** The current derive-from-`device_id` trick works only because
  the setup code is a POC placeholder (`Application.cpp:613-615` says production shows an on-device,
  rotating code). When that lands, the derive path closes and the trust anchor moves to the X25519
  `box_public_key` possession proof already on the wire. Keep the setup-code reconstruction isolated
  in one function so it can be replaced by a possession-proof handshake without touching the window
  or the worker.
- **Multi-tenant AP selection.** `select_streamer_ap()` is strongest-signal today, isolated so a
  later version can disambiguate two in-range streamers **without** factory-burned IDs (e.g.
  operator-scoped hint from the open window, or a signed streamer beacon). Unlikely near-term,
  designed-for long-term.
- **Web-UI toggle** drops on top of `GET/POST /api/provisioning-window` with no backend change.

---

## Testing

**Host unit tests (must be green before any deploy):**
- `apCredentialsForSsid`: valid `Nexus-STR-xxxxxxxx` → correct derived creds; malformed / foreign /
  injection SSIDs → fail-closed, no output. Cross-check equals `deriveApCredentials(streamer_id)`.
- Shell `select_streamer_ap()`: strongest wins; non-matching SSIDs filtered; no match → clean no-op.
- `ProvisioningWindow` state: open/close/expiry/idempotent; `seconds_remaining` correct; ttl clamp.
- Auto-pair worker (with `StubStreamerDiscovery`): pairs a discovered unpaired speaker when open;
  **refuses** when closed/expired; **skips** already-registered; derives the correct setup code;
  audit-log emitted.
- `PairingValidator` F-E: rejects malformed `streamer_id`; accepts `^STR-[0-9a-f]{8}$`.

**On-device bench (gated — explicit human go, same recover-if-it-fails safety net as prior runs):**
the real "clear a speaker's pairing → power on → auto-join → auto-pair" with the provisioning
window opened via API, behind a recovery net that restores pairing/Wi-Fi if anything fails.

## Integration point index (from code recon)

| Concern | Location |
|---|---|
| `deriveApCredentials` / `streamerIdFromApSsid` (0 callers) | `src/identity/ApCredentials.{h,cpp}` |
| Speaker `--ap-credentials` CLI | `src/main/ApCredentialsCli.{h,cpp}`, `src/main/main.cpp:42-68` |
| Boot-join script | `scripts/speaker-ap-join.sh`, `deploy/nexus-speaker-ap-join.service` |
| Pairing handshake | `streamer/src/pairing/PairingClient.h` (`pair(PairingParams)`) |
| Pairing sender wiring | `streamer/src/app/StreamerApp.cpp:173-179` |
| Streamer browse | `streamer/src/discovery/IStreamerDiscovery.h` (`browseSpeakers()`), `AvahiStreamerDiscovery.cpp` |
| `/api/pair` route | `streamer/src/web/StreamerApiRouter.cpp:369-399` |
| API auth gate | `streamer/src/web/StreamerApiRouter.cpp:26-32`; token `StreamerApp.cpp:74-88` |
| F-E validation | `src/pairing/PairingValidator.cpp:13-15`; `src/pairing/PairingService.cpp:114-132` |
| Setup-code derivation | `src/main/Application.cpp:617`; check `src/pairing/PairingValidator.cpp:24` |
| Unpaired advertise fork | `src/main/Application.cpp:608-631` |
