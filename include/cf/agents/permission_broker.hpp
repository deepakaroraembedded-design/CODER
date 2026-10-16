// cf-agents: the Permission Broker (design section 6.6.1 layer 2).
//
// Answers every opencode permission request according to policy: deny list
// first, paths confined to the agent's worktree (resolved, so `..` and
// symlinks cannot escape), simple profile commands only, and default deny.
// "Allow always" is never sent; every decision carries the matching rule id
// for the audit log. Three rejections in one turn count as a stall.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::agents {

namespace error_codes {
inline constexpr const char* kUnknownPermissionType = "PERM_UNKNOWN_TYPE";
}  // namespace error_codes

enum class PermissionReply { Once, Reject };

struct PermissionRequest {
  std::string session_id;
  std::string request_id;
  std::string role;             // coder | reviewer | qa (for the audit log)
  std::string type;             // bash, edit, read, webfetch, ...
  std::string command_or_path;  // e.g. "make -j8" or "src/registry.cpp"
  std::vector<std::string> patterns;  // opencode's patterns, used when the
                                      // command is not in the metadata
};

struct PermissionDecision {
  PermissionReply reply = PermissionReply::Reject;
  std::string rule_id;  // the policy rule that matched
  std::string reason;   // returned to the agent when rejected
};

struct PermissionPolicyConfig {
  std::string worktree;                     // absolute path of the role's worktree
  std::vector<std::string> writable_paths;  // relative: src/include or tests/qa
  // Programs the role may run (from the toolchain profile's agent_commands),
  // e.g. cmake, ninja, ctest; an entry may also be a full command, which
  // allows exactly that form (used for wrappers such as `bash -c ./configure`).
  std::vector<std::string> allowed_commands;
  // Deny entries are program names ("curl") or prefixes ("git push").
  std::vector<std::string> deny_commands;
};

class PermissionPolicy {
 public:
  explicit PermissionPolicy(PermissionPolicyConfig config);

  PermissionDecision decide(const PermissionRequest& request) const;

 private:
  PermissionDecision decide_bash(const PermissionRequest& request) const;
  PermissionDecision decide_path(const PermissionRequest& request,
                                 bool writable) const;

  PermissionPolicyConfig config_;
};

// Answers requests through a reply callback (the run process wires it to the
// opencode client) and tracks rejections per turn.
class PermissionBroker {
 public:
  using ReplyFn = std::function<void(const PermissionRequest&, PermissionDecision)>;

  PermissionBroker(PermissionPolicy policy, ReplyFn reply);

  PermissionDecision handle(const PermissionRequest& request);

  // Rejections since the last reset_turn; three count as a stall.
  std::size_t rejected_in_turn() const { return rejected_in_turn_; }
  void reset_turn() { rejected_in_turn_ = 0; }

 private:
  PermissionPolicy policy_;
  ReplyFn reply_;
  std::size_t rejected_in_turn_ = 0;
};

}  // namespace cf::agents
