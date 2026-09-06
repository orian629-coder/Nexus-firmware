# Discovery Fix — Plan (Phases 0–2)

**Status:** in progress · **Owner branch:** `fix/discovery-wiring`

**Approved scope:** version control + repo rules (Phase 0, done), speaker "find the streamer"
fix (Phase 1), streamer "advertise reliably" fix (Phase 2). **NOT** build/deploy-to-device
(Phase 3), reboot-stabilization (4), or duplication (5) — each needs its own approval.

## Symptom
On boot, paired speakers reach `SEARCHING_STREAMER` and never leave; `avahi-browse` shows no
`_nexus-*._tcp` on the LAN. Streamer and speaker are already paired in config but never connect
over the network.

## Root causes (verified — both binaries DO have Avahi; `NEXUS_STUB_HAL=OFF`)
1. **Speaker never browses.** `DiscoveryService::findStreamer()` / `browseStreamers()` has zero
   call sites; `DiscoveryService::start()` starts no browse loop; `StreamerFinder` is an empty
   stub. So `StreamerFound` — the only mDNS exit from `SEARCHING_STREAMER` — is never emitted.
   (`src/discovery/DiscoveryService.cpp:31-34,58-69`; `StreamerFinder.h:7`;
   `src/main/SystemManager.cpp:39-55`)
2. **Streamer advert never persists.** `AvahiStreamerDiscovery::advertise()` pumps the Avahi
   poll once (~100 ms) with no worker thread; the async entry-group registration never
   completes / isn't maintained. (`streamer/src/discovery/AvahiStreamerDiscovery.cpp:123-132`)
3. *(Secondary, out of this scope)* `/api/discover` returns 501 — `discoverer` passed as
   `nullptr` (`streamer/src/app/StreamerApp.cpp:180`). Deferred.

## Fix — Phase 1 (speaker: find the streamer)
- While in `SEARCHING_STREAMER`, browse `_nexus-streamer._tcp`, select the instance matching the
  stored `pairing.streamer_id`, and emit `StreamerFound` on a verified match.
- Approach (finalize in impl): an owned browse worker with retry/backoff, started on entering
  the state (`onEnter(SearchingStreamer)`) or in `DiscoveryService::start()`, stopped on exit.
- Reuse the existing, working `IDiscoveryHal::browseStreamers()` (Avahi). **No wire-contract
  change.**
- Trust check: match `streamer_id` and the `public_key` TXT against the stored
  `streamer_public_key` before emitting `StreamerFound`.

## Fix — Phase 2 (streamer: advertise reliably)
- Run the Avahi client poll on a dedicated worker thread so the entry group reaches ESTABLISHED
  and stays announced; re-announce on Avahi restart / name-collision callbacks.
- Confirm `advertise()` is invoked at startup on the `--serve` path.
- **No wire-contract change** (same type / port / TXT).

## Acceptance criteria (validatable without device deploy)
- Host-debug unit tests (StubDiscoveryHal): entering `SEARCHING_STREAMER` triggers a browse; a
  matching streamer advert → `StreamerFound` → `Authenticating`; a non-matching `streamer_id`
  is ignored.
- Streamer: unit-level assertion that `advertise()` starts a persistent poll and registers the
  entry group (mock/loopback where feasible).
- **Full on-wire proof** (`avahi-browse` shows `_nexus-streamer._tcp`; speaker reaches
  `ONLINE`) is **Phase 3** (build + deploy) — gated separately.

## Test plan
- Extend `tests/unit` and `streamer/tests/unit` following existing patterns
  (`web_router_test`, beacon/registry tests).
- `cmake --preset host-debug && ctest` must be green before proposing Phase 3.

## Out of scope here
Building/flashing devices, reboot-survival, duplication to new units, the `/api/discover` UI,
and systemd hygiene (`StartLimitIntervalSec` placement, `avahi-daemon` ordering) — all later
phases. See `../CLAUDE.md` for guardrails.
