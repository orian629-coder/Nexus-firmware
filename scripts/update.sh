#!/usr/bin/env bash
# Placeholder for the OTA software update flow. The real signature-verified, rollback-capable
# updater is implemented in the updater module (Phase 9); this script is the operator-facing
# entry point that will hand off to it. For now it performs a manual binary swap with backup.
set -euo pipefail

PREFIX=/usr/local/bin
NEW_BINARY="${1:-}"

if [[ $EUID -ne 0 ]]; then echo "update.sh must run as root" >&2; exit 1; fi
if [[ -z "$NEW_BINARY" || ! -f "$NEW_BINARY" ]]; then
  echo "Usage: update.sh <path-to-new-nexus-speaker-binary>" >&2
  exit 2
fi

# TODO(Phase 9): verify the update package signature before installing (updater/SignatureValidator).
echo "==> Backing up current binary"
cp "$PREFIX/nexus-speaker" "$PREFIX/nexus-speaker.prev"

echo "==> Installing new binary"
install -m 0755 "$NEW_BINARY" "$PREFIX/nexus-speaker"

echo "==> Restarting service"
systemctl restart nexus-speaker.service
sleep 3

if systemctl is-active --quiet nexus-speaker.service; then
  echo "==> Update OK"
else
  echo "==> Health check failed — rolling back"
  cp "$PREFIX/nexus-speaker.prev" "$PREFIX/nexus-speaker"
  systemctl restart nexus-speaker.service
  exit 1
fi
