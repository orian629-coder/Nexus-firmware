# Zero-Touch Provisioning — On-Device Bench Log (2026-09-07)

First end-to-end on-device bench of the zero-touch provisioning feature
(see [ZERO-TOUCH-PROVISIONING-DESIGN.md](./ZERO-TOUCH-PROVISIONING-DESIGN.md)).

**Outcome: the bench found a blocking bug in the unpaired bootstrap before it ever reached a
customer. Zero-touch did NOT complete end-to-end. Both speakers were recovered to their pre-test
paired state. A fix is designed (below) and pending a re-run.**

## Goal

Prove "boot → auto-join → pair" for a factory-fresh speaker: a cleared/unpaired unit, on power-up,
should (1) scan for the streamer's `Nexus-STR-<id>` AP, derive the AP credentials from the SSID
alone, and join; then (2) be auto-paired by the streamer while its provisioning window is open.

## Setup

- **streamer** `STR-a14ad83e` — AP `Nexus-STR-a14ad83e` on `10.42.0.1/24`; new binary deployed with
  `ProvisioningWindow` + `AutoPairWorker` + `GET/POST /api/provisioning-window`. Verified live:
  `GET /api/provisioning-window` → `{"open":false,"seconds_remaining":0}`.
- **speaker1** `SPK-B11A8272` (`10.42.0.50`) — CONTROL, untouched.
- **speaker2** `SPK-0AA84BEB` (`10.42.0.28`) — TEST unit. New `nexus-speaker` deployed (incl.
  `speaker-ap-join.sh` unpaired bootstrap + `--ap-credentials-for-ssid`). Full `/etc/nexus-speaker`
  + binary backed up to `/home/nexus-audio/zt-bench-backup-20260907/` first.
- Speakers are headless, **wifi-only**, reachable only through the streamer AP (jump host).

The test cleared speaker2 fully (factory-reset → `paired:false`, identity preserved; deleted the
`nexus-streamer-ap` NM profile) behind an `OnBootSec=300` auto-recovery timer, then rebooted.

## Result: two proven root causes

### 1. PRODUCT BUG — unpaired bootstrap gives up after a single Wi-Fi scan

`scripts/speaker-ap-join.sh` (unpaired branch) does **one** `nmcli … wifi rescan` ~1s into boot and
bails immediately if the AP isn't already in the scan cache. On-device journal from the failed boot:

```
22:20:55 speaker-ap-join: not paired — trying unpaired streamer-AP bootstrap
22:20:56 speaker-ap-join: no Nexus streamer AP in range — nothing to join
```

The streamer AP was up the entire time — the speaker's scan cache simply wasn't populated one second
after the service started. The **paired** path survives this because it retries *association* 3× with
a rescan each time (step 7); the **unpaired** path has no such retry on the *scan*. When the join
was skipped, `nexus-speaker` did the designed thing and raised its onboarding hotspot
(`Nexus-Setup`, open, `10.42.0.1`), leaving the unit off the streamer AP.

**Fix (designed, pending on-device re-test):** wrap the rescan + `select_streamer_ap` in a bounded
retry loop, reusing the existing `ATTEMPTS`/`BACKOFF` knobs, before giving up. Proposed diff:

```sh
# in the unpaired branch, replace the single rescan+select with:
  target_ssid=""
  for scan_try in $(seq 1 "$ATTEMPTS"); do
    nmcli -w 10 device wifi rescan ifname "$IFACE" >/dev/null 2>&1 || true
    target_ssid="$(nmcli -t -f SSID,SIGNAL dev wifi list ifname "$IFACE" 2>/dev/null | select_streamer_ap || true)"
    [ -n "$target_ssid" ] && break
    log "no Nexus streamer AP in scan ${scan_try}/${ATTEMPTS} — retrying"
    [ "$scan_try" -lt "$ATTEMPTS" ] && sleep "$BACKOFF" || true
  done
  if [ -z "$target_ssid" ]; then log "no Nexus streamer AP after ${ATTEMPTS} scans — nothing to join"; exit 0; fi
```

Not committed as live code yet: this is integration-level shell logic whose only real test is the
on-device bench, and the fleet was being restored at end of day. Apply + re-run the cold bench next
session.

### 2. TEST-SCAFFOLD BUG — auto-recovery false-positived on the shared subnet

The bench's `zt-bench-recover` safety timer was meant to restore + reboot speaker2 if it wasn't on
the AP 5 min after boot. It decided "on the AP?" by checking for a `10.42.0.x` address / a ping to
`10.42.0.1`. But the speaker's **own** onboarding hotspot also uses `10.42.0.1/24`, so the check
matched the speaker's self-address and disarmed instead of recovering:

```
22:25:49 zt-bench-recover: reachable on AP (conn='nexus-setup-ap' ip='10.42.0.1/24') — no recovery needed; disarming timer
```

The log even shows the right discriminator it ignored: `conn='nexus-setup-ap'` (own hotspot) vs
`nexus-streamer-ap` (client of the streamer AP). A correct recovery check keys off the **connection
name/mode**, never the IP — the streamer AP and the onboarding hotspot share `10.42.0.0/24`.

> Product note: the onboarding hotspot and the streamer AP sharing `10.42.0.0/24` is worth a look
> beyond the test harness — anything that reasons about "which network am I on" by address alone is
> ambiguous across those two.

## Recovery (how the fleet was restored)

speaker2 ended up parked on its own open `Nexus-Setup` hotspot, unreachable through the streamer
(separate isolated wireless network). Because the units are wifi-only and no device could join
`Nexus-Setup`, we bridged in: temporarily turned the **streamer's** wlan0 from AP into a client of
`Nexus-Setup` (control channel stayed on eth0), reached speaker2 at `10.42.0.1`, pulled the logs
above, restored `/etc/nexus-speaker` from the backup, rebooted it, then restored the streamer AP.
speaker1 (which had dropped during the AP-down window) plus speaker2 were both brought back with a
physical reboot; both rejoined via the proven paired boot-join. Final state: both `paired:true` on
the AP, `AUTHENTICATING` (pre-existing F-B).

## Device-side leftovers to clean up next session

- speaker2: `/usr/local/sbin/zt-bench-recover.sh`, `/etc/systemd/system/zt-bench-recover.{service,timer}`
  (timer already `disabled`/`inactive`), `/home/nexus-audio/zt-bench-recover.*`,
  `/home/nexus-audio/zt-cold-clear.sh`, `/home/nexus-audio/httplib.h` (harmless — lets the device
  build offline), and the backup `/home/nexus-audio/zt-bench-backup-20260907/` (keep until the
  re-run passes).
- streamer: `/home/nexus-audio/zt-bridge-{up,down}.sh` (inert). AP `autoconnect` restored to `yes`.

## Next steps

1. Apply the `speaker-ap-join.sh` scan-retry fix and rebuild/redeploy to speaker2.
2. Fix the recovery-script discriminator (connection name, not IP) before re-running the cold bench.
3. Re-run: cold-clear speaker2 → confirm unpaired bootstrap joins the AP → open the provisioning
   window → confirm `AutoPairWorker` auto-pairs it.
