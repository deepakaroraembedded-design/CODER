#include "cf/orch/session_idle_tracker.hpp"

namespace cf::orch {

bool SessionIdleTracker::record(const agents::OpencodeEvent& event) {
  if (!event.properties.is_object()) {
    return false;
  }
  const std::string session_id = event.properties.value("sessionID", "");
  if (session_id.empty()) {
    return false;
  }
  const bool idle = event.type == "session.idle";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_activity_[session_id] = std::chrono::steady_clock::now();
    if (idle) {
      ++idle_counts_[session_id];
    }
    // Track whether a tool call is running: opencode emits a part update when
    // the tool starts and another when it finishes, nothing in between.
    if (event.type == "message.part.updated") {
      const auto part = event.properties.find("part");
      if (part != event.properties.end() && part->is_object() &&
          part->value("type", "") == "tool") {
        const auto state = part->find("state");
        in_tool_[session_id] =
            state != part->end() && state->is_object() &&
            state->value("status", "") == "running";
      }
    }
  }
  if (idle) {
    cv_.notify_all();
  }
  return idle;
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

std::optional<std::chrono::steady_clock::time_point> SessionIdleTracker::last_activity(
    std::string_view session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = last_activity_.find(std::string(session_id));
  if (it == last_activity_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool SessionIdleTracker::in_tool(std::string_view session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = in_tool_.find(std::string(session_id));
  return it != in_tool_.end() && it->second;
}

}  // namespace cf::orch
