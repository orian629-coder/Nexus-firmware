# Roadmap & "what already exists"

Companion to [AGENT-HANDOFF.md](AGENT-HANDOFF.md). Answers the common first questions
("is there a GUI? do I need to build the DSP/EQ? how do I just play a sound and check it all
talks?") and lays out the next steps. Everything below was verified against the source on
`master`, not the README (which is stale in places, noted inline).

**Scope note.** The GUIs and the DSP/EQ described below already existed in the firmware; they are
not the recent connectivity work and nothing was built to "attach" to them. The recent work is the
connectivity layer (discovery, pairing, streamer-as-AP, zero-touch provisioning), which gets a
speaker as far as *discovered + paired*. The current priority (section 4, item 1) is closing the
last link so a paired speaker reaches `ONLINE` and becomes live and controllable in the existing
GUI.

## 1. What already exists (do not rebuild these)

**Three working GUIs are already in the repo:**

1. **Streamer control web UI — port `:8090`** (`streamer/src/web/StreamerFrontend.h`). This is the
   main control surface and it is feature-complete: speaker list with live telemetry, add/pair/
   remove speakers, per-speaker volume/mute/EQ-profile/delay/gain/phase/transport, a **master
   32-band EQ** + input gain + master volume, live input/output VU meters, acoustic distance
   measurement, zones. Hebrew/RTL, bearer-token auth. It runs as a plain web page served by the
   `nexus-streamer` binary — open it in a browser, no build step of its own.
2. **macOS native app** (`mac/NexusControl/`). A full **native SwiftUI** app (not a webview — the
   README calls it a WKWebView wrapper, that is stale) that mirrors the streamer web UI and also
   launches/owns the `nexus-streamer` process. Use it if you are on a Mac.
3. **Speaker local web UI — port `:8080`** (`src/web/Frontend.h`). A read-only status dashboard
   plus the Wi-Fi onboarding wizard. It does not edit volume/EQ (that lives in the streamer UI).

**A real DSP chain already exists** (`src/dsp/`, wrapped for the streamer by
`streamer/src/dsp/MasterDsp.h`): Input Gain -> Crossover -> **32-band EQ** -> Compressor ->
Limiter -> Delay -> Output Gain, plus phase-invert. The EQ is a real 32-band biquad (RBJ peaking
filters, `src/dsp/Equalizer.*`, `kEqBands = 32`), runtime-controllable over REST
(`GET/POST /api/dsp` on the streamer, `POST /api/audio/eq|volume|delay` on the speaker). So a
"20-band EQ for basic testing" is already covered and then some — nothing to add for that.

**Takeaway:** for "a basic GUI just to play sound and confirm everything communicates," you do
not build anything new. You run the streamer, open its `:8090` UI (or the Mac app), and use it.

## 2. Immediate goal: play sound + verify comms on your own bench

The end-to-end audio path is real code (streamer UDP `:50005` -> speaker -> DSP -> ALSA + amp),
but it only comes alive in a **device build** (`-DNEXUS_STUB_HAL=OFF`, the `rpi-release` preset).
On a plain host build everything is stubbed and makes no sound (that is by design, for tests).

Suggested bring-up on your own devices, in order:

1. Green host tests first: `cmake --preset host-debug && cmake --build build && ctest --test-dir build`.
2. Build + install on your speaker: `scripts/deploy.sh <user>@<your-speaker-host>` (device build,
   real HALs). If the device has no internet, pre-place `httplib.h` v0.15.3 in `/usr/local/include`
   (see AGENT-HANDOFF §3).
3. Run the streamer (`nexus-streamer --serve 8090`) or launch the Mac app, and open the `:8090` UI.
4. **Smoke-test sound directly:** `nexus-streamer --tone <speaker-ip>` sends a 2 s 440 Hz tone to
   the speaker's UDP port. The speaker plays whatever audio arrives regardless of pairing state,
   so this proves the audio path even before the control-plane is fully wired.
5. **Verify control-plane comms:** the speaker should appear in the `:8090` UI; `POST /api/status`
   does a real round-trip and returns confirmed state; the VU meters should move on audio.

## 3. Gaps and blockers to expect during bring-up

- **Under-voltage (hardware).** The original bench speakers showed Raspberry Pi under-voltage,
  which destabilises Wi-Fi and mDNS. Use a proper 5V PSU at rated amperage and a good short USB-C
  cable; confirm `vcgencmd get_throttled` is `0x0` under load before trusting Wi-Fi timing.
- **F-B: speaker stalls in `AUTHENTICATING`, never reaches `ONLINE`.** This blocks the fully
  paired/managed flow on every network. Raw `--tone` audio still works (the UDP receiver is
  independent of pairing), but the managed experience needs this fixed. Highest-value software task.
- **Some GUI buttons are acked-but-not-executed on the speaker.** Transport play/pause/stop,
  self-test, and calibration are currently deferred no-ops in the speaker's command executor
  (`src/control/CommandExecutor.cpp`) — they reply `{"accepted":true,"deferred":true}` but do
  nothing. The audio-test tone is the exception (it is bridged and does play). `SelfTest`/
  `AudioTest` are stub classes. So do not read "button did nothing" as a comms failure.

## 4. Engineering next steps (priority order)

1. **Trace and fix F-B** (`AUTHENTICATING -> ONLINE`) — the missing link between the pairing layer
   and the existing GUI. A discovered + paired speaker currently stalls in `AUTHENTICATING` and
   never becomes a live, controllable speaker in the `:8090` UI, so the connectivity work does not
   yet surface through to the GUI. Fixing this step is what connects the two. Start from
   `docs/state-machine.md` and the control/pairing path; doable on your own bench hardware.
2. **Wire up the deferred commands** so the GUI is fully live: make transport (play/pause/stop),
   self-test, and calibration actually execute on the speaker instead of acking as no-ops.
3. **Apply the zero-touch scan-retry fix** (patch in `docs/ZERO-TOUCH-BENCH-2026-09-07.md`) to
   `scripts/speaker-ap-join.sh`, add a host test, then re-run the cold provisioning bench on device.
4. **Add a one-command smoke test** (`scripts/smoke-test.sh`): tone to a speaker + `/api/status`
   round-trip + VU check, so "does this device work" is one command. None exists today.
5. **Doc hygiene:** correct the README's stale "WKWebView wrapper" description of the Mac app; keep
   this roadmap and AGENT-HANDOFF current as items land.

## 5. Can I extend the GUI from VS Code with Claude Code?

Yes. Put the repo in VS Code and ask your Claude Code to work from `master`; it should read
`AGENT-HANDOFF.md` and this file first. Extending either GUI (the `:8090` web page or the Mac app)
or adding a dedicated minimal "play + verify" panel is a normal task. But for the basic
play-sound-and-check-comms goal, start by running the existing streamer UI before building
anything new — it very likely already does what you need.
