#include "control/CommandHistory.h"

namespace nexus::control {

bool CommandHistory::seen(const std::string& command_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return results_.find(command_id) != results_.end();
}

void CommandHistory::record(const std::string& command_id, const CommandResult& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (results_.find(command_id) != results_.end()) {
    results_[command_id] = result;
    return;
  }
  if (order_.size() >= capacity_) {
    const std::string& oldest = order_.front();
    results_.erase(oldest);
    order_.pop_front();
  }
  order_.push_back(command_id);
  results_[command_id] = result;
}

bool CommandHistory::tryGet(const std::string& command_id, CommandResult& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = results_.find(command_id);
  if (it == results_.end()) return false;
  out = it->second;
  return true;
}

std::size_t CommandHistory::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return order_.size();
}

}  // namespace nexus::control
