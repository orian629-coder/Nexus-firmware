#pragma once

#include "core/StubService.h"

namespace nexus::storage {

// Owns the on-disk data tree (/var/lib/nexus-speaker) and hands out repositories. Stub in
// Phase 1 — SecureStorage is the only fully-implemented storage piece so far. SQLite-backed
// history/events land in a later phase.
NEXUS_STUB_SERVICE(LocalStorage, "storage");

}  // namespace nexus::storage
