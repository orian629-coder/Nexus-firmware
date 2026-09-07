#!/usr/bin/env bash
#
# speaker-ap-join.sh — join the paired streamer's private AP (Nexus-<streamer_id>) at boot, BEFORE
# nexus-speaker starts (see deploy/nexus-speaker-ap-join.service). Winning wlan0 first means the
# speaker boots "with network", so it never raises its onboarding setup-hotspot on wlan0 and the
# firmware's joinStreamerAp becomes a no-op — this script is the single owner of AP association.
#
# Behaviour:
#   - Unpaired (no streamer_id) -> no-op exit 0; normal onboarding proceeds.
#   - Already on the target AP  -> exit 0.
#   - Otherwise: up to N bounded rescan+connect attempts; on ANY failure, restore the last-known
#     Wi-Fi so the unit stays remotely reachable (these are headless, wifi-only client units).
#
# Credentials come from `nexus-speaker --ap-credentials` (the ONE KDF); we never re-derive here,
# never eval the output, and never log the passphrase.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}

IFACE="${NEXUS_AP_IFACE:-wlan0}"
BIN="${NEXUS_SPEAKER_BIN:-/usr/local/bin/nexus-speaker}"
CONFIG="${NEXUS_SPEAKER_CONFIG:-/etc/nexus-speaker/config.json}"
CON_NAME="${NEXUS_AP_CON:-nexus-streamer-ap}"
ATTEMPTS="${NEXUS_AP_ATTEMPTS:-3}"
BACKOFF="${NEXUS_AP_BACKOFF:-4}"
WAIT="${NEXUS_AP_WAIT:-15}"

log() { logger -t speaker-ap-join -- "$*" 2>/dev/null || true; echo "speaker-ap-join: $*"; }

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

# Fallback target, captured before we change anything so we can always get back on the air.
LAST_WIFI=""
restore_last_wifi() {
  if [ "${UNPAIRED:-0}" = "1" ]; then log "unpaired — staying on AP attempt, no Wi-Fi fallback"; return 0; fi
  if [ -n "${LAST_WIFI:-}" ]; then
    log "restoring last Wi-Fi (${LAST_WIFI})"
    nmcli -w "$WAIT" connection up "$LAST_WIFI" ifname "$IFACE" >/dev/null 2>&1 || true
  else
    log "no last Wi-Fi profile to restore"
  fi
}

# 1. Derive the paired streamer's AP credentials (single source of truth = the binary).
#    If not paired, scan for the strongest in-range Nexus streamer AP and derive creds for it
#    instead (zero-touch bootstrap); if that also fails, no-op and let normal onboarding proceed.
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

# 2. Parse WITHOUT eval, then strictly validate shape. The binary already fail-closes on a malformed
#    streamer_id, but shell must never trust CLI output unchecked: reject anything that is not exactly
#    the Nexus-STR-<8hex> SSID / 24-hex passphrase the KDF can produce.
SSID="$(printf '%s\n' "$creds" | sed -n 's/^NEXUS_AP_SSID=//p')"
PSK="$(printf '%s\n' "$creds" | sed -n 's/^NEXUS_AP_PASSPHRASE=//p')"
if ! [[ "$SSID" =~ ^Nexus-STR-[0-9a-f]{8}$ ]]; then
  log "refusing AP join: SSID from creds is not the expected Nexus-STR-<8hex> shape"
  exit 0
fi
if ! [[ "$PSK" =~ ^[0-9a-f]{24}$ ]]; then
  log "refusing AP join: passphrase from creds is not 24 hex chars"
  exit 0
fi

# 3. Already associated to the target AP? Nothing to do.
if iw dev "$IFACE" link 2>/dev/null | grep -qF -- "SSID: ${SSID}"; then
  log "already on ${SSID}"
  exit 0
fi

# 4. Remember the Wi-Fi profile active on this interface now — our fallback target.
LAST_WIFI="$(nmcli -g GENERAL.CONNECTION device show "$IFACE" 2>/dev/null || true)"

# 5. Ensure the radio is on and wlan0 is in managed/station mode (a leftover hotspot would block join).
nmcli radio wifi on 2>/dev/null || true
nmcli dev set "$IFACE" managed yes 2>/dev/null || true

# 6. Ensure a connection profile for the AP exists with the current PSK; on failure, fall back.
if nmcli -t -f NAME connection show 2>/dev/null | grep -qx "$CON_NAME"; then
  if ! nmcli connection modify "$CON_NAME" \
        wifi.ssid "$SSID" wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PSK" \
        connection.interface-name "$IFACE" connection.autoconnect yes \
        connection.autoconnect-priority 10 >/dev/null 2>&1; then
    log "failed to update AP connection profile — restoring last Wi-Fi"
    restore_last_wifi
    exit 0
  fi
else
  if ! nmcli connection add type wifi con-name "$CON_NAME" ifname "$IFACE" ssid "$SSID" \
        wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PSK" \
        connection.autoconnect yes connection.autoconnect-priority 10 >/dev/null 2>&1; then
    log "failed to add AP connection profile — restoring last Wi-Fi"
    restore_last_wifi
    exit 0
  fi
fi

# 7. Try to associate, up to ATTEMPTS, rescanning each time (each activation bounded by -w).
for n in $(seq 1 "$ATTEMPTS"); do
  nmcli dev wifi rescan ifname "$IFACE" 2>/dev/null || true
  if nmcli -w "$WAIT" connection up "$CON_NAME" ifname "$IFACE" >/dev/null 2>&1; then
    log "joined ${SSID} on attempt ${n}"
    exit 0
  fi
  log "attempt ${n}/${ATTEMPTS} to join ${SSID} failed"
  [ "$n" -lt "$ATTEMPTS" ] && sleep "$BACKOFF" || true
done

# 8. Fallback: restore last-known Wi-Fi so the unit stays reachable.
log "could not join ${SSID} after ${ATTEMPTS} attempts"
restore_last_wifi
exit 0
