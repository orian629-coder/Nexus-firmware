#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "control/Command.h"

namespace nexus::control {

// Tracks recently-seen command_ids for idempotency: a command that was already executed must not
// run again (the spec requires "do not execute the same command twice"). Bounded LRU so memory is
// capped; stores the prior result so a duplicate can be answered identically.
class CommandHistory {
 public:
  explicit CommandHistory(std::size_t capacity = 200) : capacity_(capacity) {}

  bool seen(const std::string& command_id) const;

  // Record a command_id and its result. Evicts the oldest entry when at capacity.
  void record(const std::string& command_id, const CommandResult& result);

  // Returns the stored result for a previously-seen command_id, if present.
  bool tryGet(const std::string& command_id, CommandResult& out) const;

  std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::deque<std::string> order_;                          // insertion order for eviction
  std::unordered_map<std::string, CommandResult> results_; // command_id -> result
};

}  // namespace nexus::control
