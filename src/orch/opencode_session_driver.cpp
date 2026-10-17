#include "cf/orch/opencode_session_driver.hpp"

#include <algorithm>

namespace cf::orch {

OpencodeSessionDriver::OpencodeSessionDriver(agents::OpencodeClient& client,
                                             SessionIdleTracker* tracker,
                                             agents::EventStream* stream,
                                             std::function<bool()> health)
    : client_(client), tracker_(tracker), stream_(stream), health_(std::move(health)) {}

Result<std::string> OpencodeSessionDriver::create_session() {
  auto response = client_.create_session(agents::Json::object());
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  const std::string id = response->value("id", "");
  if (id.empty()) {
    return fail(error_codes::kInvalidSession, "opencode returned a session without an id");
  }
  return id;
}

Status OpencodeSessionDriver::send_prompt(std::string_view session_id,
                                          std::string_view text) {
  if (tracker_ != nullptr) {
    // The baseline is captured before the prompt is admitted: an idle event
    // that arrives before wait_idle runs still counts for this turn.
    const std::uint64_t baseline = tracker_->idle_count(session_id);
    std::lock_guard<std::mutex> lock(baselines_mutex_);
    baselines_[std::string(session_id)] = baseline;
  }
  auto response = client_.prompt(session_id, text);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Status OpencodeSessionDriver::wait_idle(std::string_view session_id,
                                        std::chrono::milliseconds timeout,
                                        std::chrono::milliseconds stall_timeout) {
  if (tracker_ == nullptr || stream_ == nullptr) {
    return client_.wait_idle(session_id, timeout);
  }
  std::uint64_t baseline = 0;
  {
    std::lock_guard<std::mutex> lock(baselines_mutex_);
    const auto it = baselines_.find(std::string(session_id));
    if (it != baselines_.end()) {
      baseline = it->second;
    }
  }

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + timeout;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (stream_->running()) {
      // The documented turn-end signal (Phase 0a): wait for session.idle.
      auto slice = std::min(remaining, std::chrono::milliseconds(1000));
      if (stall_timeout.count() > 0) {
        slice = std::min(slice, stall_timeout);
      }
      if (tracker_->wait_for_idle(session_id, baseline, slice)) {
        return {};
      }
      if (stall_timeout.count() > 0) {
        // A hung provider stream emits nothing: no message part updates, no
        // deltas, no status events. An event from before the wait does not
        // count, so the watchdog starts at the prompt. A running tool call is
        // not a stall: builds and tests legitimately emit nothing until they
        // finish, and the overall turn timeout still bounds them.
        auto last = tracker_->last_activity(session_id);
        const auto reference =
            (last.has_value() && *last > start) ? *last : start;
        const bool tool_running =
            last.has_value() && *last > start && tracker_->in_tool(session_id);
        if (!tool_running &&
            std::chrono::steady_clock::now() - reference >= stall_timeout) {
          return fail(agents::error_codes::kTimeout,
                      "session " + std::string(session_id) + " stalled: no events for " +
                          std::to_string(stall_timeout.count()) +
                          " ms (stall watchdog)");
        }
      }
      continue;
    }
    // No event stream (fake or offline server): the status endpoint is the
    // only signal. Give a starting stream a moment to come up first.
    if (stream_->wait_running(std::min(remaining, std::chrono::milliseconds(500)))) {
      continue;
    }
    return client_.wait_idle(session_id, remaining);
  }
  return fail(agents::error_codes::kTimeout,
              "session " + std::string(session_id) + " did not become idle");
}

Status OpencodeSessionDriver::abort(std::string_view session_id) {
  auto response = client_.interrupt(session_id);
  if (!response) {
    return tl::make_unexpected(response.error());
  }
  return {};
}

Result<agents::Json> OpencodeSessionDriver::session_usage(
    std::string_view session_id) {
  return client_.session_info(session_id);
}

bool OpencodeSessionDriver::healthy() const {
  return !health_ || health_();
}

}  // namespace cf::orch
