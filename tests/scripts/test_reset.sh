#!/usr/bin/env bash
# Verifies scripts/reset.sh: wipes config/state/logs but PRESERVES the factory identity and the
# device keypair. Runs under a sandbox prefix (no root / systemd needed).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SANDBOX="$(mktemp -d)"
trap 'rm -rf "$SANDBOX"' EXIT

# Seed a "provisioned + configured" device tree.
mkdir -p "$SANDBOX/etc/nexus-speaker/identity"
mkdir -p "$SANDBOX/var/lib/nexus-speaker/secure"
mkdir -p "$SANDBOX/var/lib/nexus-speaker/calibration"
mkdir -p "$SANDBOX/var/log/nexus-speaker"

echo '{"device_id":"SPK-TEST"}' > "$SANDBOX/etc/nexus-speaker/identity/factory.json"
echo 'PRIVATE_KEY_BYTES'        > "$SANDBOX/var/lib/nexus-speaker/secure/device_ed25519_sk"
echo '{"pairing":{"paired":true}}' > "$SANDBOX/etc/nexus-speaker/config.json"
echo '{"pairing":{"paired":true}}' > "$SANDBOX/etc/nexus-speaker/config.json.bak"
echo '{"name":"room"}'          > "$SANDBOX/var/lib/nexus-speaker/calibration/room.json"
echo 'log data'                 > "$SANDBOX/var/log/nexus-speaker/speaker.log"

bash "$REPO_ROOT/scripts/reset.sh" --root "$SANDBOX" >/dev/null

fail=0
check_gone()  { [[ ! -e "$1" ]] && echo "  wiped:     $1" || { echo "  STILL EXISTS: $1" >&2; fail=1; }; }
check_kept()  { [[   -e "$1" ]] && echo "  preserved: $1" || { echo "  MISSING:      $1" >&2; fail=1; }; }

echo "== after reset =="
check_gone "$SANDBOX/etc/nexus-speaker/config.json"
check_gone "$SANDBOX/etc/nexus-speaker/config.json.bak"
check_gone "$SANDBOX/var/lib/nexus-speaker/calibration/room.json"
check_gone "$SANDBOX/var/log/nexus-speaker/speaker.log"
check_kept "$SANDBOX/etc/nexus-speaker/identity/factory.json"
check_kept "$SANDBOX/var/lib/nexus-speaker/secure/device_ed25519_sk"

if [[ $fail -ne 0 ]]; then
  echo "FAIL: reset.sh did not preserve/wipe the expected paths" >&2
  exit 1
fi
echo "PASS: reset.sh preserves identity + keypair, wipes config/state/logs"
