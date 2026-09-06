#!/usr/bin/env bash
# Stream whatever this Mac is playing to a Nexus speaker.
#
#   scripts/stream-mac.sh [speaker-ip]
#
# Route the Mac's output to BlackHole first (System Settings → Sound → Output → "BlackHole 2ch",
# or a Multi-Output Device if you also want to hear it locally). Everything the Mac plays —
# Spotify, YouTube, anything — then goes to the speaker.
#
# Capture uses a small AVFoundation helper rather than ffmpeg, so this works on a clean Mac with
# nothing installed. It emits the wire format directly (48 kHz, stereo, s16le), so the streamer
# does no conversion.
set -uo pipefail

SPEAKER="${1:-192.168.1.148}"
DEVICE="${NEXUS_CAPTURE_DEVICE:-BlackHole 2ch}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STREAMER="$REPO/build/streamer/nexus-streamer"
CAPTURE="$REPO/streamer/mac/maccapture"

if [[ ! -x "$STREAMER" ]]; then
  echo "Streamer not built. Run: cmake --build build --target nexus-streamer" >&2
  exit 1
fi

# Build the capture helper on first use so the user never has to know it exists.
if [[ ! -x "$CAPTURE" ]]; then
  echo "Building the capture helper (one time)…"
  ( cd "$REPO/streamer/mac" && swiftc -O maccapture.swift -o maccapture ) || {
    echo "Could not build the capture helper. Xcode command line tools are required." >&2
    exit 1
  }
fi

# Fail with a clear reason rather than a silent run that looks like a playback bug.
if ! curl -s --max-time 5 "http://$SPEAKER:8080/api/status" >/dev/null; then
  echo "Speaker at $SPEAKER is not responding. Check that it is on and on this network." >&2
  exit 1
fi

# Warn when the Mac is not actually routing audio to the capture device: without this the pipeline
# runs happily and streams pure silence, which is indistinguishable from a broken speaker.
if ! system_profiler SPAudioDataType 2>/dev/null \
     | grep -A6 "$DEVICE" | grep -q "Default Output Device: Yes"; then
  echo "⚠️  '$DEVICE' is not the Mac's output device — you will stream silence."
  echo "    System Settings → Sound → Output → $DEVICE   (or a Multi-Output Device)"
  echo
fi

echo "Streaming this Mac → $SPEAKER   (Ctrl-C to stop)"
"$CAPTURE" "$DEVICE" | "$STREAMER" --stream "$SPEAKER"
