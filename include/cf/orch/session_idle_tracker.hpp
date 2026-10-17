// cf-orch: event-driven turn completion (design sections 6.6.1, 6.6.2).
//
// opencode's /session/status is empty between agent steps while tools run, so
// polling cannot distinguish "between steps" from "turn finished". The
// `session.idle` event is the documented turn-end signal (Phase 0a, H1-H15);
// this tracker records it so a driver can block until the turn completes.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cf/agents/opencode_client.hpp"

namespace cf::orch {

class SessionIdleTracker {
 public:
  // Records one event; returns true when it was a session.idle event.
  bool record(const agents::OpencodeEvent& event);

  std::uint64_t idle_count(std::string_view session_id) const;

  // Blocks until the session's idle count is greater than `after`, or the
  // timeout expires. Returns false on timeout.
  bool wait_for_idle(std::string_view session_id, std::uint64_t after,
                     std::chrono::milliseconds timeout);

  // When the newest event for the session arrived (any type, not just idle).
  // Empty until an event for that session has been recorded. Drives the
  // no-events stall watchdog (design section 6.6.1 layer 3).
  std::optional<std::chrono::steady_clock::time_point> last_activity(
      std::string_view session_id) const;

  // True when the newest tool part for the session is still running: a long
  // build or test emits no events, which is not a stalled provider stream.
  bool in_tool(std::string_view session_id) const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<std::string, std::uint64_t> idle_counts_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      last_activity_;
  std::unordered_map<std::string, bool> in_tool_;
};

}  // namespace cf::orch
