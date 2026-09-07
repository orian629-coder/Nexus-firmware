#pragma once

#include <optional>
#include <string>

#include "identity/ApCredentials.h"

namespace nexus::app {

// Derive the paired streamer's private-AP credentials (SSID + WPA2 passphrase) from the speaker
// config at `config_path`, for `nexus-speaker --ap-credentials` / scripts/speaker-ap-join.sh.
//
// Pure read: never mutates the config file. Returns std::nullopt when the speaker is not paired,
// has no streamer_id, or the config cannot be read/parsed — the caller treats "no credentials" as
// "nothing to join" (let normal onboarding proceed). The derivation itself is the single source of
// truth in nexus::identity::deriveApCredentials, so speaker and streamer agree byte-for-byte.
std::optional<nexus::identity::ApCredentials> apCredentialsFromConfig(const std::string& config_path);

// Derive AP creds from a scanned SSID (unpaired bootstrap). nullopt if the
// SSID is not a well-formed Nexus streamer AP.
std::optional<nexus::identity::ApCredentials> apCredentialsForSsid(const std::string& ssid);

}  // namespace nexus::app
