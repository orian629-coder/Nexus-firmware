# Agent Handoff — start here

This page is the single starting point for whoever picks this repo up next: a new engineer, or
(most likely) a Claude Code / AI agent working on the owner's behalf. Read this first, then
`CLAUDE.md` (the guardrails), then dive into `docs/` as needed. If you read nothing else, read
the **Guardrails** and **Current state** sections below.

Plain-language summary for a non-git owner: this repository is the complete, backed-up firmware.
Everything works from the `master` branch. To make a change, you tell your Claude Code what you
want; it will read this page, build and test on its own machine, make a small change on a new
branch, and show you the result before anything touches a real device.

## 1. What this project is

C++ firmware for the **Nexus audio system**. One source tree builds **two binaries**:

- `nexus-speaker` — runs on each speaker (Raspberry Pi 4). Receives audio and signed commands.
- `nexus-streamer` — runs on the streamer (Raspberry Pi 4). The single source of commands.

They find each other on the local network (mDNS/Avahi), pair with cryptographic keys, and then
the streamer sends audio (UDP) and signed control (TCP) to the speakers. Read `README.md` for the
module map and `docs/architecture.md` for the full design.

## 2. Get the code

The default branch **`master` holds the full current code.** A fresh clone is all you need.

```sh
git clone https://github.com/orian629-coder/Nexus-firmware.git
cd Nexus-firmware
git log --oneline -5            # newest work is at the top of master
```

Branch and tag map:

| Ref | What it is |
|---|---|
| `master` | **Use this.** The complete, current firmware (all fixes + features). |
| `baseline-import` (tag) | The original code exactly as it was found on the devices, before any git work. Provenance only. |
| `fix/discovery-wiring` | Historical stage: the discovery fix, before the streamer-AP work. An ancestor of `master`. |
| `feat/streamer-ap` | Historical stage: same commit as `master` today. Kept for reference. |

You do not need the two historical branches for day-to-day work; `master` contains everything
they do. They are kept so the review history is visible.

## 3. Build and test (on your own machine, no hardware needed)

The hardware layer is stubbed on a normal computer, so the whole project builds and unit-tests
with no speaker attached. This is the loop you run for every change:

```sh
cmake --preset host-debug        # stub HAL, tests ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Dependencies (host): CMake >= 3.20, a C++17 compiler, `libsodium`, `spdlog`, `nlohmann-json`.
GoogleTest is fetched automatically. On Debian/Ubuntu:
`sudo apt install cmake g++ libsodium-dev libspdlog-dev nlohmann-json3-dev`.

**Two gotchas that will waste your time if you miss them:**

- `NEXUS_STUB_HAL` defaults **ON**. A plain host build has **no real mDNS/audio/GPIO** — that is
  correct for host tests. A real device build must pass `-DNEXUS_STUB_HAL=OFF` (the `rpi-release`
  preset does this).
- The device build pulls in `cpp-httplib` via CMake `FetchContent`, which needs **internet on the
  device**. The bench speakers have no internet (they are on the streamer's private AP), so a
  device build fails there unless `httplib.h` (v0.15.3) is pre-placed in `/usr/local/include`.
  This only affects on-device builds, never host builds.

**Green host tests are the gate.** `host-debug` + `ctest` must pass before any change is proposed
for a device.

## 4. Current state — what works, what is open

Detailed history lives in the `docs/*BENCH*` and `docs/*DESIGN*` files; this is the map.

**Working and proven:**
- Audio path, DSP chain, pairing handshake, Wi-Fi onboarding, identity, heartbeat.
- **Discovery orchestration (the original job): FIXED.** The speaker browses for the streamer and
  the streamer advertises on a worker thread; verified on-wire on real devices.
  See `docs/DISCOVERY-FIX-PLAN.md`.
- **Streamer-as-AP + boot-join: BUILT and proven on device.** The venue Wi-Fi does not forward
  client-to-client mDNS multicast, so the streamer runs its own WPA2 access point
  (`Nexus-<streamer_id>`) and each speaker joins it at boot, before the app starts, via
  `scripts/speaker-ap-join.sh`. See `docs/STREAMER-AP-DESIGN.md` and
  `docs/STREAMER-AP-BENCH-FINDINGS.md`.

**Built, host-reviewed, not yet fully proven on device:**
- **Zero-touch provisioning** (a factory-fresh speaker boots, auto-joins the streamer AP, and is
  auto-paired while a provisioning window is open). The first on-device bench found **one blocking
  bug**: the unpaired bootstrap in `scripts/speaker-ap-join.sh` does a single Wi-Fi scan ~1s into
  boot and gives up if the AP is not yet in the scan cache. **The fix is designed but not applied**
  — the exact patch (a bounded rescan-retry loop) is in `docs/ZERO-TOUCH-BENCH-2026-09-07.md`.
  Design: `docs/ZERO-TOUCH-PROVISIONING-DESIGN.md`.

**Known open issues:**
- **F-B (the current ceiling):** a paired speaker discovers the streamer but stalls in the
  `AUTHENTICATING` state and never reaches `ONLINE`, on every network. This is not a hostname or
  AP issue. It needs a dedicated trace of what drives `AUTHENTICATING -> ONLINE`. This is the
  highest-value software task open.
- **Under-voltage (hardware, not code):** the bench speakers report Raspberry Pi under-voltage
  (`vcgencmd get_throttled` returns non-zero). Under-voltage throttles the CPU and destabilises
  the Wi-Fi radio, which makes association and mDNS flaky. The fix is a proper 5V power supply at
  the rated amperage plus a good short USB-C cable, not a code change. Rule out under-voltage
  before chasing any "flaky Wi-Fi / slow discovery" symptom.

## 5. Guardrails — do not violate these

These are load-bearing. Breaking them can brick a client device or silently un-pair the fleet.

1. **Deploying to your own development devices is normal dev work — go ahead.** Host builds and
   tests are always fine too. The one hard line: do **not** flash, reboot, or reconfigure the
   customer's live/production units without explicit approval. If you are unsure whether a given
   device is a dev unit or a live one, ask before touching it.
2. **Discovery wire contract is frozen.** The mDNS service names, ports, and TXT fields
   (`docs/protocol.md`, and the box in `CLAUDE.md`) are a two-sided contract. Changing one side
   breaks interop. Any change means: change *both* sides, bump the protocol version, update
   `docs/protocol.md`.
3. **Identity and secrets are off-limits.** Device identity self-provisions on first boot
   (`SPK-...`, `STR-...`). Never regenerate it, never derive it from MAC/IP, never commit it, and
   never image a booted unit (imaging clones its identity and keys). Secrets live outside the
   source tree in `/etc/...`; never commit device config or keys. (History note: the streamer's
   identity was silently rewritten once, which un-paired every speaker — do not let this recur.)
4. **Work on a branch; keep diffs small and reviewed.** Do not commit straight to `master`.
   Green `host-debug` + `ctest` before proposing anything.
5. **Do not commit** build dirs (`build*/`), device config, keys, or `.DS_Store`. `.gitignore`
   already covers `*.key`, `identity/`, `config.local.json`, and build output.

## 6. Devices — using your own hardware

The repo owner has their **own** Nexus development devices (a streamer + speakers) for on-device
work. Building, deploying, and testing on those is expected — that is how you close the loop on
anything involving real mDNS, Wi-Fi, or audio, all of which the host build stubs out.

Deploy loop for a device:
- **Speaker:** `scripts/deploy.sh <user>@<your-speaker-host>` — rsyncs the source, builds with the
  real HALs, and runs the installer. Details in `docs/bring-up.md` and `docs/installation.md`.
- **Streamer:** currently a manual deploy — see `docs/streamer.md`.
- Device builds use the real HALs (`-DNEXUS_STUB_HAL=OFF`, the `rpi-release` preset). If the
  device has no internet (for example it is on the streamer's private AP), pre-place `httplib.h`
  v0.15.3 in `/usr/local/include` first or the `cpp-httplib` fetch fails (see section 3).

**Important — the coordinates in these docs are not your devices.** Device hostnames, IPs, and
identities mentioned here and in `CLAUDE.md` (`nexus-audio@streamer`, `10.42.0.x`, streamer
`STR-a14ad83e`, the under-voltage note on the bench speakers, etc.) come from the **original
consultant bench** and will **not** match your hardware. Treat them as reference, and point your
work at your own device coordinates.

The one caution that still holds: do not flash or reboot the **customer's live/production** units
without explicit approval (guardrail 1). Your own dev bench is yours to use freely.

## 7. Suggested next steps, in priority order

1. **Trace F-B** (`AUTHENTICATING -> ONLINE`). This blocks a speaker from ever going fully online,
   independent of the AP work. Start from the speaker state machine (`docs/state-machine.md`) and
   the control/pairing path. Highest value.
2. **Apply the zero-touch scan-retry fix** (patch in `docs/ZERO-TOUCH-BENCH-2026-09-07.md`) to
   `scripts/speaker-ap-join.sh`, add a host test alongside the existing
   `tests/scripts/speaker-ap-join-select.bats.sh`, then hand it to a human for the on-device
   re-run of the cold bench.
3. **Confirm the hardware power fix** removed under-voltage on the bench speakers before trusting
   any further Wi-Fi/discovery timing results.

## 8. Doc index

- `docs/ROADMAP.md` — what already exists (GUIs, DSP/EQ), how to play sound + verify comms on a
  bench, and the prioritized next steps. Read this if you are asked about the GUI or "what's next".
- `CLAUDE.md` — guardrails + current-status (read after this page).
- `README.md` — module map and host build.
- `docs/architecture.md`, `docs/protocol.md`, `docs/state-machine.md`, `docs/streamer.md` — design.
- `docs/DISCOVERY-FIX-PLAN.md` — the original discovery fix.
- `docs/STREAMER-AP-DESIGN.md`, `docs/STREAMER-AP-PHASE-A-PLAN.md`,
  `docs/STREAMER-AP-BENCH-FINDINGS.md` — the streamer-as-AP feature and its bench.
- `docs/ZERO-TOUCH-PROVISIONING-DESIGN.md`, `docs/ZERO-TOUCH-BENCH-2026-09-07.md` — zero-touch
  provisioning and the bench that found the open bug (with the fix patch).
