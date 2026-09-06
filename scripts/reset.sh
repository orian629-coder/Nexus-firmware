#!/usr/bin/env bash
# Factory reset for Nexus Speaker OS.
#
# IMPORTANT: this preserves the device's permanent identity. It wipes configuration, pairing,
# profiles, and runtime state — but NEVER the factory identity or the device keypair, so the same
# physical speaker re-onboards without re-issuing keys.
#
# Usage:
#   sudo reset.sh                 # real reset on the device
#   reset.sh --root <sandbox>     # operate under a prefix (testing; skips systemctl / root check)
set -euo pipefail

ROOT=""
if [[ "${1:-}" == "--root" && -n "${2:-}" ]]; then
  ROOT="$2"
fi

ETC="$ROOT/etc/nexus-speaker"
VAR="$ROOT/var/lib/nexus-speaker"
LOG="$ROOT/var/log/nexus-speaker"

if [[ -z "$ROOT" ]]; then
  if [[ $EUID -ne 0 ]]; then
    echo "reset.sh must run as root" >&2
    exit 1
  fi
  echo "==> Stopping service"
  systemctl stop nexus-speaker.service || true
fi

echo "==> Removing configuration and pairing (identity PRESERVED)"
rm -f "$ETC/config.json" "$ETC/config.json.bak" "$ETC/config.json.corrupt"

echo "==> Clearing runtime state and profiles (secure keypair PRESERVED)"
# Wipe everything under /var/lib except the secure keypair directory.
if [[ -d "$VAR" ]]; then
  find "$VAR" -mindepth 1 -maxdepth 1 ! -name secure -exec rm -rf {} + 2>/dev/null || true
fi

echo "==> Rotating logs"
rm -f "$LOG"/speaker.log* 2>/dev/null || true

echo "==> Preserved:"
echo "    $ETC/identity/          (factory.json + device certificate)"
echo "    $VAR/secure/            (device private key)"

echo "==> Reset complete. Restart with: systemctl start nexus-speaker"
