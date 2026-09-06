#!/usr/bin/env bash
# Dump a redacted diagnostic snapshot for a technician. Never prints secrets.
set -euo pipefail

ETC=/etc/nexus-speaker
LOG=/var/log/nexus-speaker/speaker.log

echo "=== nexus-speaker version ==="
/usr/local/bin/nexus-speaker --version 2>/dev/null || echo "binary not found"

echo; echo "=== service status ==="
systemctl status nexus-speaker.service --no-pager 2>/dev/null || echo "systemd not available"

echo; echo "=== device identity (public only) ==="
if [[ -f "$ETC/identity/factory.json" ]]; then
  cat "$ETC/identity/factory.json"
else
  echo "no factory identity"
fi

echo; echo "=== config (secrets are not stored in config) ==="
[[ -f "$ETC/config.json" ]] && cat "$ETC/config.json" || echo "no config"

echo; echo "=== last 40 log lines ==="
[[ -f "$LOG" ]] && tail -n 40 "$LOG" || journalctl -u nexus-speaker -n 40 --no-pager 2>/dev/null || echo "no logs"

echo; echo "=== network ==="
ip -br addr 2>/dev/null || ifconfig 2>/dev/null | grep -E "inet " || true
