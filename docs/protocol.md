# Streamer ↔ Speaker Protocol (new, signed)

The speaker is a **command server** on **TCP 45455**. The Streamer connects and sends JSON
commands, each carrying `command_id`, `expires_at`, and an Ed25519 signature. The speaker
validates signature, freshness, and idempotency before executing, and returns a result.

Implemented in Phase 3 (`control` module). This document will specify the wire format, the
mandatory command set (GET_STATUS, SET_VOLUME, SET_MUTE, SET_EQ, SET_DELAY, SET_GAIN, START_AUDIO,
STOP_AUDIO, PAUSE_AUDIO, RESUME_AUDIO, RUN_CALIBRATION, RUN_SELF_TEST, RUN_AUDIO_TEST, REBOOT,
UPDATE_SOFTWARE, RESET_NETWORK, FACTORY_RESET), discovery (mDNS beacon), and the heartbeat.
