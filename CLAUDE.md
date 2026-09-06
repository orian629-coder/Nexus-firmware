# CLAUDE.md — Working in the Nexus Firmware repo

**Read this before making changes.** This is the C++ firmware for the Nexus audio system
(speaker + streamer). It coordinates multiple contributors, including multiple Claude Code / AI
agents. Deep docs live in `docs/` (start with `docs/architecture.md`, `docs/protocol.md`,
`docs/state-machine.md`, `docs/streamer.md`). **This file is the guardrails + current-status
layer** — what to touch, what not to, and what's currently broken.

## Origin & version control
- Imported **2026-09-06** from **unversioned** on-device code. Before that it lived ONLY on the
  Pis — no git, no backup. The `streamer` device held the canonical (newest) copy; `speaker1`
  was 2 files behind (`StreamerApiRouter.cpp` + its test); `speaker2` was a stale Aug-23 snapshot.
- **Rule:** all changes go through git from now on. Never treat firmware edited directly on a
  device as the source of truth. Branch → commit → review → (approved) deploy.

## Device bench (POC)
| Host | SSH | Role / service | Web |
|---|---|---|---|
| streamer | `ssh nexus-audio@streamer` | `nexus-streamer.service` (`--serve 8090`) | :8090 |
| speaker1 | `ssh nexus-audio@speaker1` | `nexus-speaker.service` | :8080 |
| speaker2 | `ssh nexus-audio@speaker2` | `nexus-speaker.service` (stale at import) | :8080 |

Passwordless sudo on all three. **These are client devices — never deploy/flash without
explicit human approval.**

## Roles & build
- One source tree → two binaries: **`nexus-speaker`** and **`nexus-streamer`**. Role = which
  binary runs, NOT a runtime flag.
- Presets: `host-debug` (stub HAL, tests ON — local dev/CI, no Avahi/hardware) ·
  `rpi-release` / native-on-Pi with `-DNEXUS_STUB_HAL=OFF -DNEXUS_BUILD_TESTS=OFF` (real
  Avahi + hardware).
- ⚠️ **`NEXUS_STUB_HAL` defaults ON** — a plain build has NO real mDNS. Device builds MUST
  pass `-DNEXUS_STUB_HAL=OFF`.
- Deploy: speaker via `scripts/deploy.sh` + `scripts/install.sh`; **streamer deploy is
  currently manual** (`docs/streamer.md`).

## ⚠️ Discovery wire contract — DO NOT change one side only
Interop between the streamer and every speaker depends on these. Authoritative detail in
`docs/protocol.md`.
- Streamer advertises **`_nexus-streamer._tcp`** on **:8090** — TXT `streamer_id`,
  `public_key` (instance name = `streamer_id`).
- Speaker advertises **`_nexus-speaker._tcp`** on **:45455** (setup mode only) — TXT
  `device_id`, `model`, `software_version`, `box_public_key`, `setup_mode`.
- Speaker browses `_nexus-streamer._tcp`; streamer browses `_nexus-speaker._tcp`.
- Control/pairing **TCP 45455** · audio **UDP 50005**.

Changing any of these requires: change **both** sides, bump the protocol version, and update
`docs/protocol.md`.

## Identity & secrets — hands off
- Identity self-provisions on first boot: speaker `SPK-XXXXXXXX`
  (`/etc/nexus-speaker/identity/factory.json`), streamer `STR-...`. **Never** MAC/IP-derived,
  never regenerate, never commit. **Do not image a booted unit** (it clones its identity/keys).
- Secrets live outside the source tree (`/etc/...`; `/var/lib/nexus-streamer/config.json` holds
  `web.auth_token`). Never commit device config or keys.

## Current status (2026-09)
- **Working:** audio path, pairing handshake (`PairingClient`), Wi-Fi onboarding
  (`ApScanner`/`ApJoiner`, `/api/onboard-ap`), identity, heartbeat.
- **BROKEN / WIP — do not assume these work:** mDNS discovery orchestration.
  - Speaker never browses for the streamer (`findStreamer()` has no caller; `StreamerFinder`
    is an empty stub) → sits forever in `SEARCHING_STREAMER`.
  - Streamer `advertise()` is one-shot with no worker thread → nothing stays on the wire.
  - `/api/discover` returns 501 (`discoverer` passed as `nullptr`).
  - **Active fix:** `docs/DISCOVERY-FIX-PLAN.md`.

## Guardrails for automated contributors (Claude Code et al.)
- Work on a branch; never commit straight to `master`. Small, reviewed diffs.
- `host-debug` build + `ctest` must be green before proposing a deploy.
- Don't touch identity generation/storage, the wire-contract constants (except per the
  both-sides rule above), or audio/DSP internals unless that is explicitly your task.
- Don't commit build dirs (`build*/`), `.DS_Store`, device config, or keys.
- **No device deploy/flash without explicit human approval** — these are client units.
- If multiple agents/people are active, announce which module you're editing.
