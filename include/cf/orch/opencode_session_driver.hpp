// cf-orch: the SessionDriver implementation over the cf-agents opencode
// adapter (design section 6.6). One driver instance per opencode server
// (one per role).
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cf/agents/opencode_client.hpp"
#include "cf/orch/opencode_runner.hpp"
#include "cf/orch/session_idle_tracker.hpp"

namespace cf::orch {

class OpencodeSessionDriver final : public SessionDriver {
 public:
  // `tracker` and `stream` are optional: without them (or when the stream is
  // unavailable) turn completion falls back to polling /session/status.
  explicit OpencodeSessionDriver(agents::OpencodeClient& client,
                                 SessionIdleTracker* tracker = nullptr,
                                 agents::EventStream* stream = nullptr);

  Result<std::string> create_session() override;
  Status send_prompt(std::string_view session_id, std::string_view text) override;
  Status wait_idle(std::string_view session_id,
                   std::chrono::milliseconds timeout) override;
  Status abort(std::string_view session_id) override;
  Result<agents::Json> session_usage(std::string_view session_id) override;

 private:
  agents::OpencodeClient& client_;
  SessionIdleTracker* tracker_ = nullptr;
  agents::EventStream* stream_ = nullptr;
  std::mutex baselines_mutex_;
  std::unordered_map<std::string, std::uint64_t> baselines_;
};

}  // namespace cf::orch
