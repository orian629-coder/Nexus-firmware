# Streamer-AP Bench Findings — 2026-09-07

**Author:** Ariel (consulting) · **Branch:** `feat/streamer-ap` · **Status:** PAUSED for review before further code work.

On-device bench validation of the streamer-as-AP feature (Phase A). The streamer side works;
the speaker side surfaced two blocking defects that unit/host tests cannot catch, plus a
fleet-level identity problem discovered along the way. **No production rollout happened.**
speaker1 was deployed, tested, and **rolled back to its prior firmware**; speaker2 was untouched.

---

## TL;DR

| # | Finding | Severity | Blocks |
|---|---------|----------|--------|
| **F-A** | Speaker AP auto-join fails on device: `joinStreamerAp` collides with the boot-time Wi-Fi setup hotspot on `wlan0`, `nmcli` returns `code=1003 I/O error`, single-shot, no retry. | HIGH | The whole streamer-AP feature |
| **F-B** | Speaker reaches the streamer over mDNS but **stalls in `AUTHENTICATING`** — never reaches `ONLINE`, on *any* network. | HIGH | Discovery end-to-end (independent of AP) |
| **F-C** | **Streamer identity was regenerated on Aug 29**, silently un-pairing every speaker paired before then (e.g. speaker2). Violates the "identity never regenerates" rule. | HIGH | Field discovery for pre-Aug-29 pairings |
| F-D | Zero-touch "boot → auto-pair" is not implemented (pairing is operator-initiated from the streamer; unpaired speakers can't self-join the AP). Design-only. | INFO | Future zero-touch provisioning |
| **F-E** | The pairing layer accepts a peer-supplied `streamer_id` with **no format validation** (`PairingValidator` only checks non-empty). Latent injection/robustness risk for any consumer that trusts the stored value. | MEDIUM | Pairing input hardening |

---

## What is proven working ✅

- **Streamer AP.** `wlan0` runs a permanent WPA2 AP `Nexus-STR-a14ad83e` (NM `method=shared`,
  `10.42.0.1/24`) via `nexus-streamer-ap.service`; eth0 keeps internet; the streamer keeps
  advertising `_nexus-streamer._tcp` on both interfaces. Credential derivation runs correctly as
  root (the unit needs `NEXUS_STREAMER_KEY` — fixed in `df414f8`).
- **Paired boot path + AP-join wiring.** speaker1 booted the Phase A firmware cleanly
  (`SPK-B11A8272`, `NRestarts=0`), took `BOOTING → CONNECTING_NETWORK`, and `joinStreamerAp`
  fired with the **correct derived SSID** (`connecting to wifi ssid=Nexus-STR-a14ad83e`).
- **On-wire discovery.** After network was restored, the speaker **found** the streamer
  (`found streamer STR-a14ad83e at raspberrypi.local`, correct IP `192.168.1.142`) and advanced
  `SEARCHING_STREAMER → AUTHENTICATING`. Name resolution is correct (no hostname collision).
- **Lockout safety net.** A `systemd` `OnBootSec=300` timer restored venue Wi-Fi iff `wlan0` did
  not land on the AP. It fired exactly once and recovered speaker1 with zero manual action —
  validating the remote-change safety pattern for these wifi-only client units.

---

## F-A — Speaker AP auto-join fails on device (HIGH)

**Evidence (speaker1 boot journal):**

```
12:28:59.101 [provisioning] starting Wi-Fi setup hotspot (boot: no network)
12:28:59.248 [state] transition BOOTING -> CONNECTING_NETWORK (configured at boot)
12:28:59.305 [network] connecting to wifi ssid=Nexus-STR-a14ad83e         <- joinStreamerAp fired, correct SSID
12:29:00.082 [error] [network] wifi connect failed: nmcli connect failed (code=1003 I/O error)
12:29:00.089 [warning] [main] streamer-AP join failed
12:29:00.089 [state] transition CONNECTING_NETWORK -> OFFLINE (network lost)
```

**Root cause (leading hypothesis).** On a *paired* boot with no network, the speaker brings up
its **own Wi-Fi setup hotspot on `wlan0`** (0.2 s before the join). `joinStreamerAp` then tries a
station-mode `nmcli` connect on the same radio, which is already in AP mode → `I/O error`. The
onboarding hotspot is meant for *unpaired* onboarding, not for a paired speaker that should be
joining its streamer.

**Contributing factor.** `joinStreamerAp` is **single-shot**: scan once → connect once → on any
failure it drops to `OFFLINE` with no rescan/retry. Even absent the hotspot, a connect issued
~1.5 s after boot can beat NetworkManager's first scan populating the AP.

**Fix direction (no code written yet — for review):**
1. A paired speaker must **not** raise the onboarding hotspot on boot; reserve provisioning for the
   unpaired/setup path.
2. `joinStreamerAp` should ensure `wlan0` is in **station mode**, trigger a **rescan**, wait for the
   `Nexus-<id>` SSID to appear, and **retry with backoff** instead of one-shot → `OFFLINE`.
3. `CONNECTING_NETWORK` should re-enter/retry rather than terminating at `OFFLINE` on first failure.

---

## F-B — Speaker stalls in `AUTHENTICATING` (HIGH, independent of the AP)

After discovery, speaker1 transitioned `SEARCHING_STREAMER → AUTHENTICATING` and then **stopped** —
web API `state:"AUTHENTICATING"` minutes later, journal silent after the transition. It never
reached `ONLINE`. This reproduces on a **plain working network** (venue Wi-Fi), so it is unrelated
to the AP work. Ruled out: hostname collision (`raspberrypi.local` resolves correctly to the
streamer). Cause unknown — needs a dedicated trace of what drives `AUTHENTICATING → ONLINE` (does
the streamer have to connect back / heartbeat / accept the speaker?), because **discovery is
worthless until a discovered speaker can actually come online.**

---

## F-C — Streamer identity drift (HIGH, fleet-level)

| | Streamer (current) | speaker2 expects (paired Aug 23) |
|---|---|---|
| `streamer_id` | `STR-a14ad83e` | `STR-15446c90` |
| `public_key` | `+4FFfLaafnSCW+dmW+7FaorqMe/OWurr7W36G2weeME=` | `OKUCQWTAvdUSsJIb4X1Dy1Ds312HHp3dytiUDYAWzYQ=` |
| `identity.key` mtime | **Aug 29 22:33** | — |

The streamer's `/var/lib/nexus-streamer/identity.key` was **rewritten on Aug 29**, giving it a new
id + keypair. Any speaker paired before that (e.g. speaker2, paired Aug 23) now points at a
streamer identity that no longer exists: it would look for `Nexus-STR-15446c90` (wrong AP SSID) and
fail the discovery trust check on the wrong public key. speaker1 happens to be paired to the
*current* identity (re-paired after Aug 29), which is why only speaker2 is orphaned on the bench.
**Action needed:** determine *why* the identity regenerated (re-image? manual reset?) and whether
field units are affected; identity must be stable per `CLAUDE.md`.

---

## F-D — Zero-touch pairing is not implemented (INFO)

For reference (from a full code trace): an **unpaired** speaker boots into `SETUP_MODE` and
passively advertises `_nexus-speaker._tcp`; it does **not** browse for the streamer (browse is
gated on a paired `streamer_id`). Pairing is **inbound and operator-initiated** — the streamer,
via an authenticated `POST /api/pair` / `/api/onboard-ap`, connects to the speaker. There is no
code that scans for `Nexus-*` and self-joins (`streamerIdFromApSsid()` has zero callers). True
"power on → auto-pair" would be new feature work (unpaired AP-bootstrap + streamer auto-accept,
with a security guard).

---

## F-E — Pairing accepts an unvalidated `streamer_id` (MEDIUM, from code review)

A speaker's `streamer_id` is copied verbatim from the pairing peer's self-reported field
(`PairingService::persistPairing`) and `PairingValidator` only rejects it when empty — the Ed25519
signature proves key possession, not the *content* shape. The streamer always generates
`STR-<8 lowercase hex>` (`StreamerIdentity`), so anything else is malformed. This surfaced while
building the boot-join script: `streamer_id` feeds the AP SSID verbatim, and an unsanitized value
piped into shell would have been a root-RCE at boot. **Mitigated in this change** at both consumer
points — `nexus-speaker --ap-credentials` fail-closes on a malformed id, and `speaker-ap-join.sh`
parses (no `eval`) + strictly validates the SSID/passphrase shape. **Recommended systemic fix:**
validate `streamer_id` against `^STR-[0-9a-f]{8}$` in `PairingValidator`/`persistPairing` so every
future consumer of the stored value is protected, and audit other peer-supplied fields
(`streamer_public_key`) similarly.

---

## Device state after this session

| Host | Firmware | Network | Notes |
|------|----------|---------|-------|
| streamer (`STR-a14ad83e`) | New (`--ap-credentials`), **left in place** | `wlan0` = AP `Nexus-STR-a14ad83e` (10.42.0.1), eth0 internet | `nexus-streamer-ap.service` enabled + active. **Can be rolled back on request** (disable AP, restore old binary, return wlan0 to venue). |
| speaker1 (`SPK-B11A8272`) | Boot-join firmware (`951c734d…`) + `speaker-ap-join` enabled | **On the streamer AP** (`10.42.0.50`), reachable via the streamer | Boot-join proven: joined `Nexus-STR-a14ad83e` on attempt 1 before the app started (no hotspot collision); sits at `AUTHENTICATING` (F-B). venue kept as fallback. |
| speaker2 (`SPK-0AA84BEB`) | Untouched (stale) | Venue Wi-Fi (`192.168.1.20`) | Still paired to the **dead** `STR-15446c90` (see F-C). |

---

## Recommended next steps (priority order)

1. **F-B first** — trace `AUTHENTICATING → ONLINE`. Nothing else matters until a discovered
   speaker can reach `ONLINE` on a normal network.
2. **F-A** — a boot-time join script (`scripts/speaker-ap-join.sh` + `nexus-speaker-ap-join.service`,
   commit `db2554a`) now claims `wlan0` for the streamer AP **before** `nexus-speaker` starts (so the
   onboarding hotspot never collides), with 3 bounded retries + last-Wi-Fi fallback.
   **PROVEN on device (2026-09-07):** speaker1 cold-booted, `speaker-ap-join` joined the AP on
   attempt 1 before the app started (no hotspot collision), landed on `10.42.0.50`, and is reachable
   via the streamer. Remaining: F-B (still `AUTHENTICATING`), then replicate to speaker2 (needs re-pair).
3. **F-C** — investigate the Aug-29 identity regeneration and re-pair/validate field units.
4. **F-D** — decide whether zero-touch provisioning is in scope; if so, spec it as new work.

## Appendix — commits on `feat/streamer-ap`

`a59b1a3` deriveApCredentials · `6c85572` joinStreamerAp · `6995a2b` Application auto-join wiring ·
`ba2f2d6` `--ap-credentials` CLI · `18a7777` `streamer-ap.sh` · `2c6ca5c` unit + docs ·
`837ca11` final fixes · `df414f8` AP unit `NEXUS_STREAMER_KEY` fix (caught on device) ·
`06d2326` these findings · `db2554a` speaker boot-time streamer-AP join (F-A fix, this doc's step 2).
