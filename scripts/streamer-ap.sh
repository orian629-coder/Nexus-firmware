#!/usr/bin/env bash
# streamer-ap.sh — bring the permanent private "Nexus-<streamer_id>" Wi-Fi AP up or down.
#
# The streamer hosts this AP so speakers join a clean, streamer-controlled network where mDNS
# multicast works (the venue AP drops client-to-client multicast — see docs/STREAMER-AP-DESIGN.md).
# Uses NetworkManager AP mode (method=shared → NM runs dnsmasq for DHCP+DNS), WPA2-secured, with
# NO captive portal (unlike the speaker setup hotspot). SSID + passphrase come from the binary's
# identity-derived KDF, so the derivation lives in exactly one place.
#
# Usage: streamer-ap.sh up | down   (normally driven by nexus-streamer-ap.service at boot)
set -euo pipefail
export PATH="/usr/sbin:/sbin:$PATH"

CON="nexus-streamer-ap"
IFACE="${NEXUS_AP_IFACE:-wlan0}"
BIN="${NEXUS_STREAMER_BIN:-/usr/local/bin/nexus-streamer}"

load_creds() {
  # Exports NEXUS_AP_SSID / NEXUS_AP_PASSPHRASE from the binary's derivation.
  eval "$("$BIN" --ap-credentials)"
  if [[ -z "${NEXUS_AP_SSID:-}" || -z "${NEXUS_AP_PASSPHRASE:-}" ]]; then
    echo "streamer-ap: failed to derive AP credentials from $BIN" >&2
    exit 1
  fi
}

case "${1:-}" in
  up)
    load_creds
    nmcli connection delete "$CON" >/dev/null 2>&1 || true
    # One-shot hotspot: sets AP mode, band, WPA2, and shared IPv4 (NM's own dnsmasq) correctly.
    nmcli device wifi hotspot ifname "$IFACE" con-name "$CON" \
      ssid "$NEXUS_AP_SSID" password "$NEXUS_AP_PASSPHRASE" >/dev/null
    # Persist + autoconnect so the AP returns after a reboot even without this unit re-running.
    nmcli connection modify "$CON" connection.autoconnect yes
    echo "streamer AP up: SSID='$NEXUS_AP_SSID' on $IFACE (WPA2, method=shared)"
    ;;
  down)
    nmcli connection down "$CON" >/dev/null 2>&1 || true
    nmcli connection delete "$CON" >/dev/null 2>&1 || true
    echo "streamer AP down: $IFACE released"
    ;;
  *)
    echo "usage: streamer-ap.sh up|down" >&2
    exit 1
    ;;
esac
