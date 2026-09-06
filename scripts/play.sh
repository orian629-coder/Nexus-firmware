#!/usr/bin/env bash
# Play an audio file from this Mac to a Nexus speaker.
#
#   scripts/play.sh <file> [speaker-ip]
#   scripts/play.sh ~/Music/song.mp3
#   scripts/play.sh ~/Music/song.mp3 192.168.1.148
#
# The streamer decodes WAV itself, but nothing else — so anything that is not already
# 16-bit PCM WAV is converted first with afconvert, which ships with macOS. That keeps this
# working on a clean machine with no ffmpeg, no brew, and no other install.
set -euo pipefail

FILE="${1:-}"
SPEAKER="${2:-192.168.1.148}"

if [[ -z "$FILE" ]]; then
  echo "Usage: scripts/play.sh <audio-file> [speaker-ip]" >&2
  exit 1
fi
if [[ ! -f "$FILE" ]]; then
  echo "No such file: $FILE" >&2
  exit 1
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STREAMER="$REPO/build/streamer/nexus-streamer"
if [[ ! -x "$STREAMER" ]]; then
  echo "Streamer not built. Run: cmake --build build --target nexus-streamer" >&2
  exit 1
fi

# Fail early with a clear reason rather than a silent no-audio run.
if ! curl -s --max-time 5 "http://$SPEAKER:8080/api/status" >/dev/null; then
  echo "Speaker at $SPEAKER is not responding. Check that it is powered on and on this network." >&2
  exit 1
fi

PLAYFILE="$FILE"
CLEANUP=""
# Convert unless it is already a WAV the streamer can read directly. afconvert targets the wire
# format exactly (48 kHz, stereo, little-endian 16-bit) so the streamer does no resampling.
if [[ "${FILE##*.}" != "wav" && "${FILE##*.}" != "WAV" ]]; then
  PLAYFILE="$(mktemp -t nexusplay).wav"
  CLEANUP="$PLAYFILE"
  echo "Converting to 48 kHz stereo…"
  afconvert -f WAVE -d LEI16@48000 -c 2 "$FILE" "$PLAYFILE"
fi
trap '[[ -n "$CLEANUP" ]] && rm -f "$CLEANUP"' EXIT

echo "Playing $(basename "$FILE") → $SPEAKER"
echo "(Ctrl-C to stop)"
NEXUS_STREAMER_CONFIG="${NEXUS_STREAMER_CONFIG:-$HOME/.nexus-streamer/config.json}" \
  "$STREAMER" --play "$PLAYFILE" --tone "$SPEAKER"
