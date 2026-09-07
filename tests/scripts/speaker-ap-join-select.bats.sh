#!/usr/bin/env bash
# Self-contained test for select_streamer_ap(): sources the function out of the
# join script and feeds it mocked `nmcli dev wifi` output on stdin.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/../../scripts/speaker-ap-join.sh"

# Extract just the function body (script guards real actions behind a main guard).
# shellcheck disable=SC1090
source <(sed -n '/^select_streamer_ap()/,/^}/p' "$SCRIPT")

fail() { echo "FAIL: $1" >&2; exit 1; }

# strongest of two Nexus APs wins
out="$(printf '%s\n' 'Nexus-STR-a14ad83e:41' 'Nexus-STR-deadbeef:88' 'HandsomeWiFi:90' | select_streamer_ap)"
[ "$out" = "Nexus-STR-deadbeef" ] || fail "strongest: got '$out'"

# foreign / malformed SSIDs are filtered out
out="$(printf '%s\n' 'Nexus-Setup:99' 'CafeWiFi:99' 'Nexus-STR-BADCAPS:99' | select_streamer_ap || true)"
[ -z "$out" ] || fail "filtering: got '$out'"

# exactly one match
out="$(printf '%s\n' 'CafeWiFi:70' 'Nexus-STR-a14ad83e:55' | select_streamer_ap)"
[ "$out" = "Nexus-STR-a14ad83e" ] || fail "single: got '$out'"

echo "OK"
