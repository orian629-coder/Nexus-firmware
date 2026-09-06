#pragma once

#include <string>

#include "core/Result.h"

namespace nexus::core {

// Atomically replace `path` with `contents`: write a temp sibling, fsync it, rename over the
// target, then fsync the containing directory so the rename itself survives a power cut.
//
// This is the write primitive behind every persisted file on the device — a partially written
// config after a power failure is the difference between a speaker that boots and one that does
// not. It lived inside config/ConfigManager.cpp's anonymous namespace; it was lifted here so the
// streamer's own config manager reuses the exact same implementation instead of duplicating a
// subtly different one.
Status atomicWriteFile(const std::string& path, const std::string& contents);

}  // namespace nexus::core
