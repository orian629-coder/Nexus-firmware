#!/usr/bin/env bash
# setup-ntp.sh — provision a shared clock for sample-aligned multi-room playback.
#
# The audio wire protocol carries an ABSOLUTE playback timestamp: every speaker releases a packet
# when its own clock reaches that time. So all devices must agree on the wall clock to within a few
# milliseconds. On a normal LAN with internet, plain chrony against public NTP already achieves this.
# On an isolated LAN (no internet), the streamer host runs a local NTP server and the speakers sync
# to it. This script configures chrony for either role. Idempotent; run as root on Linux (the Pi).
#
# Usage:
#   sudo scripts/setup-ntp.sh server                 # on the streamer host (serves the LAN)
#   sudo scripts/setup-ntp.sh client <streamer-ip>   # on each speaker (syncs to the streamer)
#   sudo scripts/setup-ntp.sh client                 # speaker, sync to public NTP only
#
# Note: the streamer usually runs on a Mac in this phase. macOS uses its own timesync, not chrony —
# there, either keep the Mac and speakers on the same public NTP (internet present), or run the
# server role on one of the Pis and point the others (and the Mac's `sntp`) at it. See docs/streamer.md.
set -euo pipefail

ROLE="${1:-}"
STREAMER_IP="${2:-}"
CONF=/etc/chrony/chrony.conf
LAN_ALLOW="${NEXUS_NTP_ALLOW:-192.168.0.0/16}"   # subnet the server answers; override via env

if [[ $EUID -ne 0 ]]; then
  echo "setup-ntp.sh must run as root" >&2
  exit 1
fi

if [[ "$ROLE" != "server" && "$ROLE" != "client" ]]; then
  echo "usage: setup-ntp.sh server | client [<streamer-ip>]" >&2
  exit 1
fi

echo "==> Installing chrony"
if command -v apt-get >/dev/null 2>&1; then
  apt-get update && apt-get install -y chrony
else
  echo "setup-ntp.sh: no apt-get; install chrony manually" >&2
fi

# Chrony's conf path differs across distros; fall back to /etc/chrony.conf.
[[ -f "$CONF" ]] || CONF=/etc/chrony.conf
mkdir -p "$(dirname "$CONF")"

# Keep a one-time backup of the distro default.
[[ -f "${CONF}.nexus-orig" ]] || { [[ -f "$CONF" ]] && cp "$CONF" "${CONF}.nexus-orig"; }

if [[ "$ROLE" == "server" ]]; then
  echo "==> Configuring chrony as a LAN time server (allow $LAN_ALLOW)"
  cat > "$CONF" <<EOF
# Managed by nexus setup-ntp.sh (server role).
# Upstream sources when internet is available; harmless if it isn't.
pool 2.debian.pool.ntp.org iburst
# Serve time to the local speakers.
allow ${LAN_ALLOW}
# Serve our own clock as a last resort so speakers still converge on an isolated LAN.
local stratum 10
driftfile /var/lib/chrony/chrony.drift
makestep 1.0 3
rtcsync
EOF
else
  echo "==> Configuring chrony as a client"
  {
    echo "# Managed by nexus setup-ntp.sh (client role)."
    if [[ -n "$STREAMER_IP" ]]; then
      echo "# Prefer the streamer's clock so multi-room aligns even without internet."
      echo "server ${STREAMER_IP} iburst prefer minpoll 2 maxpoll 4"
    fi
    echo "pool 2.debian.pool.ntp.org iburst"
    echo "driftfile /var/lib/chrony/chrony.drift"
    echo "makestep 1.0 3"
    echo "rtcsync"
  } > "$CONF"
fi

echo "==> Restarting chrony"
systemctl enable chrony 2>/dev/null || systemctl enable chronyd 2>/dev/null || true
systemctl restart chrony 2>/dev/null || systemctl restart chronyd 2>/dev/null || true

echo "==> Done. Verify with:  chronyc tracking   and   chronyc sources"
[[ "$ROLE" == "server" ]] && echo "    Point speakers at this host:  sudo scripts/setup-ntp.sh client <this-ip>"
