#pragma once

#include <string>
#include <vector>

namespace nexus::streamer::group {

// A named set of speakers that plays one synchronized stream ("קומה ראשונה", "Ground Floor").
//
// Zones exist ONLY in the streamer. The speaker firmware has no concept of them: it is always
// addressed individually by device_id, and grouping is expressed by sending the same byte-identical
// timestamped datagrams to every member. That identity is the whole basis of multi-room sync, so a
// zone is really two things at once — a command fan-out set and an audio fan-out set.
//
// The two sets are deliberately not the same, though. Audio skips members that are offline (sending
// to a dead host is wasted bandwidth), while commands are still sent to them so the user gets a real
// per-speaker error instead of silence. See ZoneManager::resolveTargets vs ::members.
struct Zone {
  std::string zone_id;                // stable generated id ("zone-xxxxxxxx")
  std::string name;                   // human label
  std::vector<std::string> members;   // device_ids
};

}  // namespace nexus::streamer::group
