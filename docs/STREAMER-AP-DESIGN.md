# Streamer-as-AP — Design (private `Nexus-Audio` network)

**Status:** DESIGN — approved in brainstorming, not yet planned/implemented ·
**Owner branch:** `feat/streamer-ap` · **Date:** 2026-09-06 ·
**Depends on:** discovery fix Phases 1–2 (`fix/discovery-wiring`) — the browse/advertise wiring
this design relies on is already done there.

## Why (problem this solves)

Discovery Phases 1–2 fixed the firmware: the streamer advertises `_nexus-streamer._tcp` reliably
and a paired speaker browses for and connects to it. **Verified on device.** But on the shop's
WiFi ("Handsome" AP) the service never propagates: `speaker1` resolves the streamer's *hostname*
(unicast mDNS crosses) yet never sees the *service* (multicast mDNS does not). Confirmed
environmental, not firmware — both Pis sit on the *same* AP with a strong signal, WiFi power-save
was disabled, and it still fails. The AP filters / isolates client-to-client multicast. See
`DISCOVERY-FIX-PLAN.md` Phase 3.

Depending on an uncontrolled venue AP for mDNS multicast is fragile by nature. This design removes
that dependency: the **streamer runs its own AP** and the speakers live on it, so discovery happens
on a clean, controlled L2 subnet where multicast works.

## Decisions (from brainstorming, 2026-09-06)

1. **Audio/topology reality:** the streamer downloads audio from the cloud (needs internet),
   pre-fetches, and relays to speakers over the local link; speakers also record via mics for
   spatial/latency localization. **Speakers need no internet of their own** — they only receive the
   relay and do local mic work. → Speakers can live entirely on a streamer-run AP.
2. **Streamer uplink:** **wired `eth0` primary**; single-radio concurrent **AP+STA "juggle"**
   fallback when no cable. The juggle is feasibility-gated (see §5).
3. **Speaker AP onboarding:** **identity-derived** SSID + passphrase (chosen for simplicity /
   zero-config). Residual security trade-off accepted for the POC and recorded in §2.

## 1. Topology

- **Streamer** runs a permanent **WPA2 AP** on `wlan0` via NetworkManager `method=shared`
  (NM's own dnsmasq for DHCP+DNS) — the same mechanism `scripts/hotspot.sh` already uses for the
  speaker setup portal, but **permanent, WPA2-secured, and without** the captive portal / internet
  isolation.
- **Speakers** join `Nexus-Audio` as their sole network (off venue WiFi entirely). On this subnet
  the mDNS discovery from Phases 1–2 works unchanged — dedicated bandwidth and predictable jitter,
  which *helps* the pre-emptive relay + mic-timing model.
- **Streamer internet:** `eth0` → venue router. If speakers ever need internet, the streamer
  NATs the AP subnet out `eth0` (NM shared mode already masquerades).

```
              venue router
                   │ (ethernet, primary)
                 eth0
             ┌──────────┐
             │ STREAMER │  wlan0 = AP "Nexus-Audio" (WPA2, method=shared)
             └──────────┘
                 (( ))  private subnet 10.42.0.0/24 (NM shared default)
            ┌──────┴───────┐
        speaker1        speaker2   ← join Nexus-Audio; discover streamer via mDNS here
```

## 2. Identity-derived credentials

- **SSID = `Nexus-<streamer_id>`** (e.g. `Nexus-STR-a14ad83e`), broadcast by the streamer.
- **Passphrase = KDF(streamer_id)** — deterministic, ≥8-char WPA2-valid, computed identically on
  both sides from the `streamer_id` alone. (Concrete KDF chosen at implementation: a fixed-salt
  hash of `streamer_id` rendered to a WPA2-safe charset; must be stable across firmware versions —
  it is part of the join contract.)
- **Speaker join flow:** scan for `Nexus-*` → read `streamer_id` from the SSID → derive passphrase
  → join. No typing, no pre-pairing needed to get *onto* the network; pairing + discovery then run
  on the AP subnet. A freshly-onboarded speaker therefore no longer needs venue-WiFi at all.

### Residual security trade-off (accepted for POC, flagged)

Because the passphrase derives from the `streamer_id` (which is in the SSID) via a known function,
anyone who knows the derivation can compute it — the AP is effectively "open to insiders." This is
acceptable at the POC/closed-venue stage. **Later hardening** (out of scope here): a per-unit
*random* passphrase sealed to the speaker during the existing pairing handshake (encrypted with the
speaker's public key), so the secret never rides the SSID. Tracked as a known limitation, not a
silent one.

## 3. Firmware / config changes (small — mostly reuse)

- **Streamer:** a `nexus-streamer-ap` NetworkManager profile (autoconnect, `method=shared`, WPA2,
  derived SSID/passphrase) installed at deploy and brought up at boot — modeled on `hotspot.sh`.
  Minimal or no C++ (a script + unit + install step; a small hook in `StreamerApp` only if boot
  ordering needs it).
- **Speaker:** derive-creds-and-join logic over the existing `nexus::network::NetworkManager` /
  `INetworkHal` (`scan()` + `connectWifi()`), preferring `Nexus-*` over venue WiFi, wired on the
  `CONNECTING_NETWORK` state edge. (`ApScanner`/`ApJoiner` are streamer-only — not used here.)
- **Discovery:** **unchanged.** The wire contract (`_nexus-streamer._tcp` :8090, `_nexus-speaker.
  _tcp` :45455, TCP 45455, UDP 50005) is untouched. Nothing in the Phase 1–2 work changes.

## 4. Uplink selection

At streamer boot: if `eth0` has carrier → **wired mode** (`wlan0` is a pure AP). Else → **juggle
mode** (§5). A small selection step (script/service level).

## 5. No-cable "juggle" fallback — feasibility-gated

Concurrent **AP+STA** on the single onboard radio: the firmware time-shares one chip between a
client interface (venue WiFi, for the streamer's internet) and the `Nexus-Audio` AP. The links stay
continuously up — we do **not** hand-roll tear-down/rebuild alternation (that would drop speaker
associations every switch and wreck the audio timing).

**Hard constraints:** both interfaces are forced onto the **venue AP's channel**; throughput roughly
halves; stability depends on the Pi chip/firmware and is **unproven for the multi-speaker audio
load**. → **Go/no-go spike on the real streamer Pi** to prove it sustains the relay *before* this
fallback is designed in. Wired mode has none of these caveats and ships first regardless.

## 6. Scope / phasing

- **Phase A (build now):** wired-mode streamer AP + identity-derived creds + speaker auto-join;
  discovery runs over the AP. Fully testable, low-risk, solves the mDNS blocker end-to-end.
- **Phase B (spike → then decide):** the concurrent AP+STA juggle fallback.

## 7. Testing

- **Host-debug unit tests:** credential derivation (deterministic KDF — same input → same
  SSID/passphrase, WPA2-valid output) and any profile/args builder. Stub HAL, no systemctl (per
  the local-testing rule — do NOT run full-Application e2e tests locally; they trigger polkit
  dialogs).
- **On-wire proof** (AP up; speaker joins `Nexus-Audio`; speaker reaches `ONLINE`) is a **gated
  device-deploy step**, like discovery Phase 3 — needs explicit approval; these are client units.
- The **juggle spike** is manual on hardware.

## 8. Risks & open items

- **R1 — juggle feasibility (high):** AP+STA on the Pi's onboard chip may not sustain the audio
  load. Mitigation: spike before build; wired-mode is independent of this.
- **R2 — first-boot bootstrapping:** a brand-new speaker must reach the streamer to pair; with
  speakers on the streamer AP this happens *on* the AP subnet after the scan/derive/join — confirm
  no chicken-and-egg with the current pairing entry point during planning.
- **R3 — identity-derived secret (accepted):** see §2 residual trade-off.
- **R4 — losing venue-WiFi management access:** moving speakers off venue WiFi changes how they're
  reached for SSH/ops; confirm the ops story (reach speakers via the streamer AP / streamer as a
  jump host) during planning.

## 9. Out of scope

Per-unit sealed-pairing passphrase (future hardening), the juggle fallback build (spike first),
`/api/discover` UI (501, deferred), reboot-stabilization and duplication (discovery Phases 4–5),
and any change to the discovery wire contract or identity generation. See repo `CLAUDE.md` for
guardrails.

## 10. Guardrails honored

Branch-based (`feat/streamer-ap`); wire contract and identity generation untouched; `host-debug` +
tests green before any deploy; **no device deploy/flash without explicit human approval.**
