# Nexus Control (macOS app)

A native Mac window that wraps the streamer's control UI — add / scan / pair / control network
speakers from a real app in the Dock instead of a browser tab.

It launches and owns `nexus-streamer --serve` for you: on open it starts the server, waits for it,
shows the UI in a `WKWebView`, and on quit it kills the streamer child (no leaked background server).

## Build

```
# 1. Build the streamer binary once (it gets bundled into the .app)
cmake --preset host-debug && cmake --build build --target nexus-streamer

# 2. Build the app
mac/NexusControl/build.sh            # → mac/NexusControl/build/Nexus Control.app
mac/NexusControl/build.sh --open     # build and launch
```

Then double-click **Nexus Control.app** (or drag it to `/Applications`).

## How the streamer is found at runtime

In order: `$NEXUS_STREAMER_BIN` → bundled `Contents/MacOS/nexus-streamer` → repo `build/streamer/…`
→ `/usr/local/bin/nexus-streamer`. If none is found the app still opens and tries to attach to an
already-running `--serve` on port 8090.

## Notes

- Uses port 8090 (matches the streamer default). Change `kPort` in `main.swift` if needed.
- The UI, speaker registry, pairing, and network scan all come from the streamer itself — this app
  is only the window + process lifecycle. See [../../docs/streamer.md](../../docs/streamer.md).
