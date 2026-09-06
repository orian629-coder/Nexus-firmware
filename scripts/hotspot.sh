#!/usr/bin/env bash
# hotspot.sh — bring a Wi-Fi setup Access Point + captive portal up or down.
#
# During onboarding (no network) the speaker runs a Wi-Fi AP named "Nexus-Setup" on wlan0 so a
# phone can join it and open the /setup page to hand over the real Wi-Fi credentials. Uses
# NetworkManager's built-in AP mode (method=shared → it runs dnsmasq for DHCP + DNS itself), so no
# extra daemons are needed. The captive-portal redirect (all DNS → the speaker) is done by pointing
# NetworkManager's shared DNS at ourselves; the web server answers every host with the setup page.
#
# Usage:
#   hotspot.sh up      # start the AP + portal
#   hotspot.sh down    # tear it down, freeing wlan0 to join a real network
#
# Idempotent. Invoked by nexus-speaker's ProvisioningController (via the nexus-hotspot unit), not by
# hand in normal operation.
set -euo pipefail

# systemd runs services with a minimal PATH (/usr/local/bin:/usr/bin:/bin) that OMITS /usr/sbin,
# where `nft` lives on Debian. Without this, `command -v nft` fails and the captive-portal redirect
# + internet-isolation silently no-op — so the setup page never auto-pops. Put the sbin dirs on PATH.
export PATH="/usr/sbin:/sbin:$PATH"

CON="nexus-setup-ap"
SSID="${NEXUS_HOTSPOT_SSID:-Nexus-Setup}"
IFACE="${NEXUS_HOTSPOT_IFACE:-wlan0}"
# The setup AP is OPEN — no password. Onboarding is the first thing a user does, and typing a
# password off the screen was the main friction point; the AP is short-lived, has no route to the
# internet (captive_isolate_up) and only serves the /setup page, so the exposure is a stranger
# reaching that page. Set NEXUS_HOTSPOT_PASSWORD to a >=8 char string to force WPA back on.
PASSWORD="${NEXUS_HOTSPOT_PASSWORD:-}"
# The AP's own address (NetworkManager's shared-mode default); the /setup page lives here.
AP_ADDR="10.42.0.1"

# Captive-portal probes hit port 80 (http://captive.apple.com, /generate_204, …) but the web server
# listens on 8080. Redirect port 80 on the AP interface to 8080 so those probes reach the server,
# which 302s them to /setup and pops the setup page open automatically on the phone.
#
# Debian 13 (trixie) ships nftables and no longer installs iptables by default, so we drive nft
# natively and only fall back to iptables where it exists. A missing redirect must NOT abort the
# script (set -e) — the AP itself is already up and usable; the phone just won't auto-pop the page.
NFT_TABLE="nexus_captive"
redirect_up() {
  if command -v nft >/dev/null 2>&1; then
    nft add table ip "$NFT_TABLE" 2>/dev/null || true
    nft "add chain ip $NFT_TABLE prerouting { type nat hook prerouting priority dstnat ; }" 2>/dev/null || true
    # Flush our chain first so re-running up is idempotent (no duplicate rules).
    nft flush chain ip "$NFT_TABLE" prerouting 2>/dev/null || true
    nft add rule ip "$NFT_TABLE" prerouting iifname "$IFACE" tcp dport 80 redirect to :8080 \
      || echo "warning: nft redirect failed; captive-portal auto-open disabled" >&2
  elif command -v iptables >/dev/null 2>&1; then
    iptables -t nat -C PREROUTING -i "$IFACE" -p tcp --dport 80 -j REDIRECT --to-ports 8080 \
      2>/dev/null || \
      iptables -t nat -A PREROUTING -i "$IFACE" -p tcp --dport 80 -j REDIRECT --to-ports 8080 \
      || echo "warning: iptables redirect failed; captive-portal auto-open disabled" >&2
  else
    echo "warning: neither nft nor iptables present; captive-portal auto-open disabled" >&2
  fi
}
redirect_down() {
  if command -v nft >/dev/null 2>&1; then
    nft delete table ip "$NFT_TABLE" 2>/dev/null || true
  fi
  if command -v iptables >/dev/null 2>&1; then
    iptables -t nat -D PREROUTING -i "$IFACE" -p tcp --dport 80 -j REDIRECT --to-ports 8080 \
      2>/dev/null || true
  fi
}

# Robust fallback for the captive portal: a tiny userspace forwarder listening on port 80 that
# proxies to the web UI on 8080. The nftables redirect above is the fast path but proved fragile
# across NetworkManager versions; this guarantees http://10.42.0.1 always reaches /setup.
FWD_SCRIPT="/usr/local/bin/nexus-port80-forward.py"
FWD_PID="/run/nexus-port80.pid"
forwarder_up() {
  if [[ ! -f "$FWD_SCRIPT" ]]; then
    echo "warning: port-80 forwarder missing ($FWD_SCRIPT) — captive portal may not auto-open" >&2
    return 0
  fi
  forwarder_down
  nohup python3 "$FWD_SCRIPT" >/var/log/nexus-port80.log 2>&1 &
  echo $! > "$FWD_PID"
  # Give it a moment to bind :80, then confirm — this is the captive-portal path when nft is absent,
  # so a silent bind failure (e.g. another listener on :80) must be surfaced, not swallowed.
  sleep 0.3
  if ! kill -0 "$(cat "$FWD_PID" 2>/dev/null)" 2>/dev/null; then
    echo "warning: port-80 forwarder failed to start — see /var/log/nexus-port80.log" >&2
  fi
}
forwarder_down() {
  [[ -f "$FWD_PID" ]] && kill "$(cat "$FWD_PID")" 2>/dev/null || true
  rm -f "$FWD_PID"
  pkill -f "$FWD_SCRIPT" 2>/dev/null || true
}

# Captive DNS: make the hotspot's dnsmasq resolve EVERY name to the AP. This is what makes phones
# auto-pop the setup window — iOS/Android probe a known URL on connect; when its DNS points here and
# the page isn't the expected "success", the OS shows the captive-portal window automatically.
# Dropped into NetworkManager's shared-dnsmasq include dir; only affects the hotspot connection.
DNSMASQ_DIR="/etc/NetworkManager/dnsmasq-shared.d"
captive_dns_up() {
  mkdir -p "$DNSMASQ_DIR"
  printf 'address=/#/%s\n' "$AP_ADDR" > "$DNSMASQ_DIR/nexus-captive.conf"
}
captive_dns_down() {
  rm -f "$DNSMASQ_DIR/nexus-captive.conf"
}

# Block internet for hotspot clients during onboarding. NetworkManager's shared mode masquerades
# wlan0 clients out through eth, giving them REAL internet — which makes iOS/Android decide "there's
# no captive portal here" and NOT pop the setup window. Dropping forwarded traffic that leaves the
# hotspot subnet makes the OS see "no internet" and auto-open the captive page. Clients can still
# reach the AP itself (10.42.0.1) for DHCP/DNS/the setup page, since that is INPUT to the Pi, not
# forwarded. The Pi's own eth uplink (our SSH) is unaffected.
CAPTIVE_FWD_TABLE="nexus_captive_fwd"
captive_isolate_up() {
  command -v nft >/dev/null 2>&1 || return 0
  nft add table ip "$CAPTIVE_FWD_TABLE" 2>/dev/null || true
  nft "add chain ip $CAPTIVE_FWD_TABLE forward { type filter hook forward priority -10 ; }" 2>/dev/null || true
  nft flush chain ip "$CAPTIVE_FWD_TABLE" forward 2>/dev/null || true
  # Drop anything forwarded FROM the hotspot subnet to a destination OUTSIDE it (i.e. the internet).
  nft add rule ip "$CAPTIVE_FWD_TABLE" forward iifname "$IFACE" ip daddr != 10.42.0.0/24 drop \
    2>/dev/null || true
}
captive_isolate_down() {
  command -v nft >/dev/null 2>&1 || return 0
  nft delete table ip "$CAPTIVE_FWD_TABLE" 2>/dev/null || true
}

case "${1:-}" in
  up)
    # NetworkManager's dedicated hotspot command sets AP mode, band, WPA, and shared IPv4 (its own
    # dnsmasq for DHCP+DNS) correctly in one shot. Building the profile by hand made NM demand a WEP
    # secret ("Secrets were required"), so we use the dedicated command with a known password.
    nmcli connection delete "$CON" >/dev/null 2>&1 || true
    captive_dns_up   # must exist before NM starts the shared dnsmasq
    if [[ -n "$PASSWORD" ]]; then
      nmcli device wifi hotspot ifname "$IFACE" con-name "$CON" ssid "$SSID" password "$PASSWORD" \
        >/dev/null
    else
      # `nmcli device wifi hotspot` always provisions WPA and rejects an empty password, so create
      # the profile through it (it gets AP mode, band and shared IPv4 right) and then drop the
      # security setting entirely, re-activating so the change takes effect.
      #
      # The whole 802-11-wireless-security setting must be REMOVED. Clearing the individual
      # properties does not work: `wifi-sec.key-mgmt ""` is rejected outright ("property is
      # missing") and `wifi-sec.key-mgmt none` means WEP, not open — both leave the AP demanding a
      # password while the script happily reports success. Hence no `|| true` here: if this fails
      # we want the failure to surface, not a silently-WPA "open" hotspot.
      nmcli device wifi hotspot ifname "$IFACE" con-name "$CON" ssid "$SSID" password "nexussetup" \
        >/dev/null
      nmcli connection modify "$CON" remove 802-11-wireless-security >/dev/null
      nmcli connection up "$CON" >/dev/null
    fi
    redirect_up
    forwarder_up
    captive_isolate_up   # no real internet for clients → OS pops the captive window
    # Verify the AP really came up with the security we asked for. A profile that silently kept WPA
    # would broadcast an SSID the QR advertises as open, so the phone fails to join with a useless
    # "could not join network" — exactly the failure this check exists to catch.
    sec="$(nmcli -g 802-11-wireless-security.key-mgmt connection show "$CON" 2>/dev/null || true)"
    if [[ -z "$PASSWORD" && -n "$sec" ]]; then
      echo "warning: setup AP still has security '$sec' — phones will be asked for a password" >&2
    fi
    echo "hotspot up: SSID='$SSID' ${PASSWORD:+pass='$PASSWORD' }on $IFACE at $AP_ADDR ($([[ -n "$PASSWORD" ]] && echo WPA || echo open), captive portal → /setup)"
    ;;
  down)
    redirect_down
    forwarder_down
    captive_dns_down
    captive_isolate_down
    nmcli connection down "$CON" >/dev/null 2>&1 || true
    nmcli connection delete "$CON" >/dev/null 2>&1 || true
    echo "hotspot down: $IFACE released"
    ;;
  *)
    echo "usage: hotspot.sh up|down" >&2
    exit 1
    ;;
esac
