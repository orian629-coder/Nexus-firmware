#pragma once

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/Event.h"

namespace nexus::streamer::events {

// Streamer-local event vocabulary.
//
// The streamer shares the speaker's EventBus implementation (core::EventBus) but NOT its event
// catalog: core::EventType is a closed enum owned by the speaker, so adding a streamer-specific
// value there would force a speaker-side edit for every streamer concern and couple the two
// modules in the wrong direction. Instead every streamer event rides core::EventType::StateChanged
// and carries its discriminator in Event::source. Subscribers filter on source, not on type.
//
// Payloads must never contain secrets (keys, Wi-Fi PSKs) — same rule as the speaker's bus.

constexpr const char* kSpeakerOnline = "speaker.online";
constexpr const char* kSpeakerOffline = "speaker.offline";
constexpr const char* kSpeakerState = "speaker.state";  // confirmed state changed (Phase 2)
constexpr const char* kZoneChanged = "zone.changed";    // Phase 4
constexpr const char* kStreamStarted = "stream.started";
constexpr const char* kStreamStopped = "stream.stopped";
constexpr const char* kAlertRaised = "alert.raised";

// Build a streamer event. `name` must be one of the constants above so subscribers can match it.
inline nexus::core::Event make(const char* name, nlohmann::json data = nlohmann::json::object()) {
  return nexus::core::Event{nexus::core::EventType::StateChanged, name, std::move(data)};
}

// True when `ev` is the streamer event called `name`. Use this in subscribers rather than comparing
// Event::type, which is StateChanged for every streamer event.
inline bool is(const nexus::core::Event& ev, const char* name) {
  return ev.type == nexus::core::EventType::StateChanged && ev.source == name;
}

}  // namespace nexus::streamer::events
