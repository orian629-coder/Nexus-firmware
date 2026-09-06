# Operations

- **Install**: `sudo ./scripts/install.sh` (creates the service user, dirs, polkit rule, systemd
  unit; never overwrites an existing config or identity).
- **Start / logs**: `systemctl start nexus-speaker` / `journalctl -u nexus-speaker -f`.
- **Diagnostics**: `./scripts/diagnostics.sh` (redacted snapshot).
- **Factory reset**: `sudo ./scripts/reset.sh` — clears config/pairing/state, **preserves device
  identity and keypair**.
- **Update**: `sudo ./scripts/update.sh <new-binary>` (signature-verified OTA lands in Phase 9).

Paths: config `/etc/nexus-speaker/config.json`, identity `/etc/nexus-speaker/identity/`, secrets
`/var/lib/nexus-speaker/secure/`, logs `/var/log/nexus-speaker/speaker.log` + journal.
