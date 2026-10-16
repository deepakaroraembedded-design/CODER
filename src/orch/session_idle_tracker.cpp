#include "cf/orch/session_idle_tracker.hpp"

namespace cf::orch {

bool SessionIdleTracker::record(const agents::OpencodeEvent& event) {
  if (event.type != "session.idle" || !event.properties.is_object()) {
    return false;
  }
  const std::string session_id = event.properties.value("sessionID", "");
  if (session_id.empty()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++idle_counts_[session_id];
  }
  cv_.notify_all();
  return true;
}

std::uint64_t SessionIdleTracker::idle_count(std::string_view session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = idle_counts_.find(std::string(session_id));
  return it == idle_counts_.end() ? 0 : it->second;
}

bool SessionIdleTracker::wait_for_idle(std::string_view session_id, std::uint64_t after,
                                       std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  const std::string key(session_id);
  return cv_.wait_for(lock, timeout, [&] {
    const auto it = idle_counts_.find(key);
    return it != idle_counts_.end() && it->second > after;
  });
}

}  // namespace cf::orch
