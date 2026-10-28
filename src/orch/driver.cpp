#include "cf/orch/driver.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cf/sandbox/docker.hpp"
#include "cf/sandbox/namespaces.hpp"
#include "cf/telemetry/logging.hpp"
#include "cf/orch/waivers.hpp"

namespace cf::orch {
namespace {

const char* role_for_phase(MilestonePhase phase) {
  switch (phase) {
    case MilestonePhase::AuthorTests: return "qa";
    case MilestonePhase::Code: return "coder";
    case MilestonePhase::Fix: return "coder";
    case MilestonePhase::Review: return "reviewer";
    case MilestonePhase::Validate: return "qa";
    case MilestonePhase::Integrate: return "orchestrator";
  }
  return "orchestrator";
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

bool is_public_interface_path(std::string_view path) {
  return starts_with(path, "include/") && (ends_with(path, ".h") || ends_with(path, ".hpp"));
}

bool is_qa_owned_path(std::string_view path) {
  return starts_with(path, "tests/") || starts_with(path, "qa/");
}

// The failing gates of the milestone's latest gate run with their first
// finding, so the fix brief carries the actual failure instead of making the
// agent dig it out (design section 7.3).
std::string failing_gate_summary(const store::RunStore& store,
                                 const std::string& run_id,
                                 const std::string& milestone_id) {
  auto rows = store.latest_gate_results(run_id, milestone_id);
  if (!rows.has_value()) {
    return {};
  }
  std::string out;
  for (const auto& row : *rows) {
    if (row.passed) {
      continue;
    }
    std::string message;
    std::ifstream in(row.report_ref, std::ios::binary);
    if (in) {
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      const auto report = nlohmann::json::parse(text, nullptr, false);
      if (report.is_object()) {
        for (const auto& result :
             report.value("results", nlohmann::json::array())) {
          if (result.value("gate", "") != row.gate ||
              result.value("passed", true)) {
            continue;
          }
          for (const auto& finding :
               result.value("findings", nlohmann::json::array())) {
            message = finding.value("message", "");
            break;
          }
          break;
        }
      }
    }
    if (message.empty()) {
      message = "failed (see get_gate_results)";
    }
    if (message.size() > 300) {
      message.resize(300);
    }
    if (!out.empty()) {
      out += "; ";
    }
    out += row.gate + ": " + message;
  }
  return out;
}

// Build and cache directories an agent may create while working; they are
// never committed by the snapshot, so they do not count as out-of-scope
// changes (design section 6.13.2).
bool is_build_artifact(std::string_view path) {
  static const std::set<std::string_view> ignored = {
      "build",        "out",           "dist",          "target",
      "node_modules", "__pycache__",   ".cache",        ".pytest_cache",
      ".mypy_cache",  ".ruff_cache",   "cmake-build-debug", "cmake-build-release"};
  const auto slash = path.find('/');
  const std::string_view first =
      slash == std::string_view::npos ? path : path.substr(0, slash);
  if (ignored.count(first) > 0) {
    return true;
  }
  // Agents name build trees in many ways: build, build2, build_strict,
  // build-debug, cmake-build-release.
  return first.rfind("build", 0) == 0 || first.rfind("cmake-build", 0) == 0;
}

// True when a changed path is inside the milestone's declared write set
// (design section 6.13.2): an exact entry or a path below a directory entry.
bool within_write_set(const std::vector<std::string>& writes, std::string_view path) {
  for (const auto& entry : writes) {
    if (path == entry) {
      return true;
    }
    if (path.size() > entry.size() && path.compare(0, entry.size(), entry) == 0 &&
        path[entry.size()] == '/') {
      return true;
    }
  }
  return false;
}

}  // namespace

WorkspaceDriver::WorkspaceDriver(plan::Plan plan, DriverConfig config,
                                 git::Repository& repository,
                                 process::ProcessExecutor& executor,
                                 AgentRunner& agent_runner, store::RunStore* store)
    : plan_(std::move(plan)),
      config_(std::move(config)),
      repository_(repository),
      executor_(executor),
      agent_runner_(agent_runner),
      store_(store) {}

std::filesystem::path WorkspaceDriver::worktree_for(std::string_view milestone_id,
                                                    std::string_view role) const {
  return config_.workspace_root / "wt" / config_.run_id / std::string(milestone_id) /
         std::string(role);
}

std::filesystem::path WorkspaceDriver::integration_worktree() const {
  return config_.workspace_root / "wt" / config_.run_id / "integration";
}

std::string WorkspaceDriver::start_point() const {
  auto exists = repository_.branch_exists(config_.integration_branch);
  if (exists.has_value() && *exists) {
    return config_.integration_branch;
  }
  return config_.base_branch;
}

Result<std::filesystem::path> WorkspaceDriver::ensure_worktree(
    std::string_view milestone_id, std::string_view role) {
  const std::filesystem::path path = worktree_for(milestone_id, role);
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    return path;
  }
  const std::string branch = "cf-local/" + config_.run_id + "/" +
                             std::string(milestone_id) + "/" + std::string(role);
  // Test-first: the coder starts from the QA test commit when it exists
  // (design sections 6.9, 7.3).
  std::string start = start_point();
  if (role == "coder") {
    const std::string qa_branch = "cf-local/" + config_.run_id + "/" +
                                  std::string(milestone_id) + "/qa";
    auto qa_exists = repository_.branch_exists(qa_branch);
    if (qa_exists.has_value() && *qa_exists) {
      start = qa_branch;
    }
  }
  auto created = repository_.create_worktree(path, branch, start);
  if (!created) {
    return tl::make_unexpected(created.error());
  }
  if (role == "coder") {
    auto head = repository_.head_sha(path);
    if (head) {
      base_sha_[std::string(milestone_id)] = *head;
    }
  }
  return path;
}

Status WorkspaceDriver::snapshot(std::string_view milestone_id, std::string_view role,
                                 const std::vector<std::string>& paths,
                                 const std::string& message, const std::string& op_id) {
  auto worktree = ensure_worktree(milestone_id, role);
  if (!worktree) {
    return tl::make_unexpected(worktree.error());
  }
  std::vector<std::string> existing;
  for (const auto& path : paths) {
    std::error_code ec;
    if (std::filesystem::exists(*worktree / path, ec)) {
      existing.push_back(path);
    }
  }
  if (existing.empty()) {
    return fail(git::error_codes::kNoChanges,
                "no snapshot paths exist in " + worktree->string());
  }
  git::CommitTrailers trailers;
  trailers.run_id = config_.run_id;
  trailers.milestone_id = std::string(milestone_id);
  trailers.waivers = waivers_trailer(milestone_id);
  trailers.op_id = op_id;
  auto sha = repository_.commit_paths(*worktree, existing, message, trailers);
  if (!sha) {
    return tl::make_unexpected(sha.error());
  }
  return {};
}

std::string WorkspaceDriver::waivers_trailer(
    std::string_view milestone_id) const {
  if (store_ == nullptr) {
    return "none";
  }
  auto waivers = store_->list_waivers(config_.run_id, milestone_id);
  if (!waivers || waivers->empty()) {
    return "none";
  }
  std::string joined;
  for (const auto& waiver : *waivers) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += waiver.rule + "@" + waiver.scope;
  }
  return joined;
}

AgentTurnRequest WorkspaceDriver::make_request(MilestonePhase phase,
                                               std::string_view milestone_id,
                                               std::string_view role) const {
  AgentTurnRequest request;
  request.phase = phase;
  request.run_id = config_.run_id;
  request.milestone_id = std::string(milestone_id);
  request.role = std::string(role);
  request.worktree = worktree_for(milestone_id, role).string();
  // The coder works against the milestone's open defects (design section 12).
  if (store_ != nullptr) {
    auto defects = store_->list_defects(config_.run_id, milestone_id,
                                        store::open_defect_statuses());
    if (defects.has_value()) {
      for (const auto& defect : *defects) {
        request.open_defect_ids.push_back(defect.id);
      }
    }
  }
  const auto join = [](const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
      if (!out.empty()) {
        out += "; ";
      }
      out += value;
    }
    return out;
  };
  if (const plan::MilestoneSpec* spec = plan_.find_milestone(milestone_id)) {
    request.requirement_ids = spec->requirements;
    switch (phase) {
      case MilestonePhase::AuthorTests:
        request.brief =
            "Author the test plan and tests for milestone " +
            std::string(milestone_id) + " (" + spec->title +
            "). The tests are expected to fail until the coder implements the "
            "milestone, so do not file defects for the missing implementation";
        break;
      case MilestonePhase::Code:
        request.brief = "Implement milestone " + std::string(milestone_id) + " (" +
                        spec->title + ")";
        break;
      case MilestonePhase::Fix:
        request.brief = "Fix the failing gates and open defects for milestone " +
                        std::string(milestone_id);
        break;
      case MilestonePhase::Review:
        request.brief = "Review the milestone " + std::string(milestone_id) +
                        " diff against the design, contracts and guardrails";
        break;
      case MilestonePhase::Validate:
        request.brief = "Validate milestone " + std::string(milestone_id) +
                        " and file defects for any failure";
        break;
      case MilestonePhase::Integrate:
        request.brief = "Integrate milestone " + std::string(milestone_id);
        break;
    }
    // The agents work from the plan's acceptance criteria and constraints
    // (design section 6.6.1): without them the brief is only a title.
    if (!spec->acceptance.empty()) {
      request.brief += ". Acceptance criteria: " + join(spec->acceptance);
    }
    if (!plan_.forbidden.empty()) {
      request.brief += ". Forbidden: " + join(plan_.forbidden);
    }
    // The coder must stay inside the milestone's declared write set (design
    // section 6.13.2); the snapshot rejects anything outside it.
    if ((phase == MilestonePhase::Code || phase == MilestonePhase::Fix) &&
        !spec->writes.empty()) {
      request.brief += ". Write set (only these paths may change): " +
                       join(spec->writes);
    }
  }
  // The fix brief carries the failing gates so the agent works from the
  // failure instead of rediscovering it (design section 7.3).
  if (phase == MilestonePhase::Fix && store_ != nullptr) {
    const std::string failures =
        failing_gate_summary(*store_, config_.run_id, std::string(milestone_id));
    if (!failures.empty()) {
      request.brief += ". Failing gates from the last run: " + failures;
    }
  }
  // Validation decides the milestone's exit criteria (design section 11.1):
  // every open defect must be re-tested and verified or the milestone cannot
  // pass, whatever the tests say.
  if (phase == MilestonePhase::Validate && !request.open_defect_ids.empty()) {
    request.brief += ". Re-test the open defects (" +
                     join(request.open_defect_ids) +
                     ") and mark each VERIFIED with verify_defect when the failure "
                     "no longer reproduces; the milestone cannot pass while any "
                     "defect is open";
  }
  return request;
}

void WorkspaceDriver::record_checkpoint(std::string_view milestone_id) {
  if (store_ == nullptr) {
    return;
  }
  nlohmann::json heads = nlohmann::json::object();
  for (const char* role : {"coder", "qa", "reviewer"}) {
    const std::filesystem::path path = worktree_for(milestone_id, role);
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
      continue;
    }
    auto head = repository_.head_sha(path);
    if (head.has_value()) {
      heads[role] = *head;
    }
  }
  store::CheckpointRecord checkpoint;
  checkpoint.run_id = config_.run_id;
  checkpoint.milestone_id = std::string(milestone_id);
  checkpoint.worktree_heads_json = heads.dump();
  checkpoint.sessions_json = "[]";  // sessions are abandoned on recovery
  const auto milestone = store_->get_milestone(config_.run_id, milestone_id);
  checkpoint.milestone_state = milestone.has_value() ? milestone->state : "";
  checkpoint.at = store::unix_now();
  const auto saved = store_->save_checkpoint(checkpoint);
  if (!saved.has_value()) {
    spdlog::warn("cannot record the checkpoint for {}: {}", milestone_id,
                 saved.error().message);
  }
}

Result<std::filesystem::path> WorkspaceDriver::ensure_integration_worktree() {
  const std::filesystem::path path = integration_worktree();
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    auto created = repository_.create_worktree(path, config_.integration_branch,
                                               config_.base_branch);
    if (!created) {
      return tl::make_unexpected(created.error());
    }
  }
  return path;
}

Status WorkspaceDriver::prepare_codeintel() {
  if (config_.codeintel == nullptr) {
    return {};  // no back end for the plan's language: indexing is off
  }
  auto worktree = ensure_integration_worktree();
  if (!worktree) {
    return tl::make_unexpected(worktree.error());
  }
  auto head = repository_.head_sha(*worktree);
  if (!head) {
    return tl::make_unexpected(head.error());
  }
  // The index is advisory context, never a gate: a refresh failure is
  // logged, not fatal.
  auto refreshed = config_.codeintel->refresh_to(*head);
  if (!refreshed) {
    spdlog::warn("codeintel: refresh to {} failed: {}", head->substr(0, 12),
                 refreshed.error().message);
  } else if (*refreshed > 0) {
    spdlog::info("codeintel: indexed {} file(s) at {}", *refreshed,
                 head->substr(0, 12));
  }
  return {};
}

TurnResult WorkspaceDriver::run(MilestonePhase phase, std::string_view milestone_id) {
  telemetry::ScopedContext log_context(config_.run_id, std::string(milestone_id),
                                       role_for_phase(phase));
  if (phase == MilestonePhase::Integrate) {
    // The integration worktree lives under wt/<run>/integration.
    auto path = ensure_integration_worktree();
    if (!path) {
      return TurnResult{false, "", path.error().message, false, false, false, false,
                        false, ""};
    }
    auto before = repository_.head_sha(*path);
    if (!before) {
      return TurnResult{false, "", before.error().message, false, false, false, false,
                        false, ""};
    }
    const std::string branch = "cf-local/" + config_.run_id + "/" +
                               std::string(milestone_id) + "/coder";
    git::CommitTrailers trailers;
    trailers.run_id = config_.run_id;
    trailers.milestone_id = std::string(milestone_id);
    trailers.waivers = waivers_trailer(milestone_id);
    trailers.op_id = store::new_op_id();
    trailers.squashed_from = branch;
    auto merged = repository_.merge_squash(*path, branch,
                                           "feat(" + std::string(milestone_id) + "): " +
                                               std::string(milestone_id),
                                           trailers);
    if (!merged) {
      if (merged.error().code == git::error_codes::kMergeConflict) {
        (void)repository_.abort_merge(*path);
        TurnResult conflict;
        conflict.completed = true;
        conflict.merged = false;
        conflict.conflicts = true;
        conflict.detail = merged.error().message;
        return conflict;
      }
      if (merged.error().code == git::error_codes::kNoChanges) {
        // Nothing to integrate: the branch's content is already in the
        // integration tree. That is the normal end of a revalidation cycle
        // (design section 6.13.3) — and, after a crash between the merge
        // and its state record, the merge row of design section 6.12.5: the
        // integration branch already carries this milestone's squash merge
        // (its CF-Milestone trailer), so confirm instead of failing.
        auto landed = repository_.find_commit_by_trailer(
            config_.integration_branch, "CF-Milestone",
            std::string(milestone_id));
        if (integrated_.count(std::string(milestone_id)) > 0 ||
            (landed.has_value() && !landed->empty())) {
          integrated_.insert(std::string(milestone_id));
          revalidating_.erase(std::string(milestone_id));
          TurnResult done;
          done.completed = true;
          done.merged = true;
          done.summary = "already integrated";
          return done;
        }
      }
      return TurnResult{false, "", merged.error().message, false, false, false, false,
                        false, ""};
    }
    integrated_.insert(std::string(milestone_id));
    revalidating_.erase(std::string(milestone_id));
    auto changes = repository_.diff(*before, *merged);
    std::vector<std::string> changed_interfaces;
    if (changes.has_value()) {
      for (const auto& change : *changes) {
        if (is_public_interface_path(change.path)) {
          changed_interfaces.push_back(change.path);
        }
      }
    }
    changed_interfaces_[std::string(milestone_id)] = changed_interfaces;
    if (config_.codeintel != nullptr) {
      // Keep the symbol index in step with the integration head (design
      // section 6.5). Advisory: a refresh failure never blocks the merge.
      auto refreshed = config_.codeintel->refresh_to(*merged);
      if (!refreshed) {
        spdlog::warn("codeintel: refresh to {} failed: {}", merged->substr(0, 12),
                     refreshed.error().message);
      }
    }
    TurnResult result;
    result.completed = true;
    result.merged = true;
    result.interface_changed = !changed_interfaces.empty();
    result.summary = "merged " + branch;
    return result;
  }

  const std::string role =
      phase == MilestonePhase::Review ? "reviewer"
                                      : (phase == MilestonePhase::AuthorTests ||
                                                 phase == MilestonePhase::Validate
                                             ? "qa"
                                             : "coder");
  // QA validation checks the coder's committed work, so it runs in the
  // coder's worktree: the QA worktree never received the implementation.
  // Test authoring stays in the QA worktree (design sections 6.9, 7.3). A
  // revalidating milestone is already merged, so its review and validation
  // run against the integration worktree (design section 6.13.3).
  const bool revalidating = revalidating_.count(std::string(milestone_id)) > 0;
  // Review and validation of a revalidating milestone look at the merged
  // result; a fix works on the milestone's branch (synced below).
  const bool integration_view =
      revalidating &&
      (phase == MilestonePhase::Review || phase == MilestonePhase::Validate);
  Result<std::filesystem::path> worktree =
      integration_view
          ? ensure_integration_worktree()
          : ensure_worktree(milestone_id,
                            phase == MilestonePhase::Validate ? "coder" : role);
  if (!worktree) {
    return TurnResult{false, "", worktree.error().message, false, false, false, false,
                      false, ""};
  }

  // A fix during revalidation targets the new integration head: the
  // milestone's work is already merged, so sync the coder worktree to the
  // integration before the turn (design section 6.13.3). Without this the
  // agent would fix against the pre-change interface.
  if (phase == MilestonePhase::Fix &&
      integrated_.count(std::string(milestone_id)) > 0) {
    auto integration = ensure_integration_worktree();
    if (integration) {
      auto head = repository_.head_sha(*integration);
      if (head) {
        auto reset = repository_.reset_hard(*worktree, *head);
        if (!reset) {
          return TurnResult{false, "", reset.error().message, false, false, false,
                            false, false, ""};
        }
        spdlog::info("milestone {}: revalidation fix starts from the integration head",
                     milestone_id);
      }
    }
  }

  // The pre-turn checkpoint (design section 6.12.3): a crash during the turn
  // resets the worktrees to this commit instead of trying to continue a
  // half-finished model turn.
  record_checkpoint(milestone_id);

  AgentTurnRequest request = make_request(phase, milestone_id, role);
  request.worktree = worktree->string();
  auto outcome = agent_runner_.run_turn(request);
  if (!outcome) {
    spdlog::warn("{} turn for {} failed: {}", role, milestone_id,
                 outcome.error().message);
    return TurnResult{false, "", outcome.error().message, false, false, false, false,
                      false, ""};
  }
  if (!outcome->completed) {
    return TurnResult{false, "", outcome->detail, false, false, false, false, false, ""};
  }

  switch (phase) {
    case MilestonePhase::AuthorTests: {
      auto stored = snapshot(milestone_id, role, {"tests", "qa"},
                             "test(" + std::string(milestone_id) + "): " +
                                 outcome->summary,
                             store::new_op_id());
      if (!stored) {
        return TurnResult{false, "", stored.error().message, false, false, false, false,
                          false, ""};
      }
      TurnResult result;
      result.completed = true;
      result.summary = outcome->summary;
      return result;
    }
    case MilestonePhase::Code:
    case MilestonePhase::Fix: {
      // Write-set enforcement (design section 6.13.2): a change outside the
      // milestone's declared write set is rejected before it is committed, so
      // the scheduler's parallel-safety guarantees hold. Plans without
      // declared write sets (no refinement) are not restricted, and the
      // snapshot commits exactly the declared set.
      std::vector<std::string> snapshot_paths = {"src", "include"};
      if (const plan::MilestoneSpec* spec = plan_.find_milestone(milestone_id)) {
        if (!spec->writes.empty()) {
          snapshot_paths = spec->writes;
          auto changes = repository_.worktree_changes(*worktree);
          if (!changes) {
            return TurnResult{false, "", changes.error().message, false, false, false,
                              false, false, ""};
          }
          std::vector<std::string> outside;
          for (const auto& path : *changes) {
            if (is_build_artifact(path)) {
              continue;
            }
            if (!within_write_set(spec->writes, path)) {
              outside.push_back(path);
            }
          }
          if (!outside.empty()) {
            const auto join_paths = [](const std::vector<std::string>& values) {
              std::string joined;
              for (const auto& value : values) {
                if (!joined.empty()) {
                  joined += ", ";
                }
                joined += value;
              }
              return joined;
            };
            return TurnResult{false, "",
                              "changes outside the declared write set for " +
                                  std::string(milestone_id) + ": " +
                                  join_paths(outside) + " (declared: " +
                                  join_paths(spec->writes) +
                                  "); move them inside the declared set and "
                                  "submit again",
                              false, false, false, false, false, ""};
          }
        }
      }
      const std::string prefix = phase == MilestonePhase::Code ? "feat" : "fix";
      auto stored = snapshot(milestone_id, role, snapshot_paths,
                             prefix + "(" + std::string(milestone_id) + "): " +
                                 outcome->summary,
                             store::new_op_id());
      // A dispute-only turn may have changed nothing, and so may a fix on an
      // already-integrated milestone (the work is merged; the turn resolved a
      // dispute or reviewed comments). Both proceed to the gates. A no-op
      // submission for ordinary, unmerged work is still a failed turn.
      if (!stored) {
        const bool no_changes = stored.error().code == git::error_codes::kNoChanges;
        const bool permitted =
            no_changes && (!outcome->disputed_test_id.empty() ||
                           integrated_.count(std::string(milestone_id)) > 0);
        if (!permitted) {
          return TurnResult{false, "", stored.error().message, false, false, false,
                            false, false, ""};
        }
      }
      // New work means the milestone is back to its normal cycle: the next
      // gate run targets its own branch, not the integration (design section
      // 6.13.3).
      revalidating_.erase(std::string(milestone_id));
      TurnResult result;
      result.completed = true;
      result.summary = outcome->summary;
      // A turn that ended with dispute_test routes to the QA decision before
      // the next gate run (design section 12).
      result.disputed_test_id = outcome->disputed_test_id;
      return result;
    }
    case MilestonePhase::Review: {
      TurnResult result;
      result.completed = true;
      result.summary = outcome->summary;
      result.changes_requested = outcome->changes_requested;
      return result;
    }
    case MilestonePhase::Validate: {
      TurnResult result;
      result.completed = true;
      result.summary = outcome->summary;
      result.exit_criteria_met = outcome->exit_criteria_met;
      return result;
    }
    case MilestonePhase::Integrate:
      break;  // handled above
  }
  return TurnResult{false, "", "unreachable phase", false, false, false, false, false, ""};
}

// The QA decision turn for an open test dispute (design sections 6.9, 12):
// QA fixes the protected test or rejects the dispute. The turn runs in the
// coder's worktree so a corrected test is what the next gate run builds, and
// the fix is committed there (tests/ and qa/ are QA-owned paths).
TurnResult WorkspaceDriver::run_test_dispute(std::string_view milestone_id) {
  telemetry::ScopedContext log_context(config_.run_id, std::string(milestone_id),
                                       "qa");
  const std::string id(milestone_id);
  auto worktree = ensure_worktree(milestone_id, "coder");
  if (!worktree) {
    return TurnResult{false, "", worktree.error().message, false, false, false, false,
                      false, ""};
  }
  record_checkpoint(milestone_id);

  std::string brief =
      "Decide the open test dispute(s) for milestone " + id +
      ". The tests under tests/ are QA-owned: if a disputed test is wrong (for "
      "example it cannot compile against the agreed interface), fix it and mark "
      "its dispute defect VERIFIED with verify_defect. If the test is correct, "
      "reject the dispute by leaving it open. ";
  if (store_ != nullptr) {
    auto defects = store_->list_defects(config_.run_id, milestone_id,
                                        store::open_defect_statuses());
    if (defects.has_value()) {
      for (const auto& defect : *defects) {
        if (defect.status != "DISPUTED") {
          continue;
        }
        const auto body = nlohmann::json::parse(defect.body_json, nullptr, false);
        if (!body.is_object()) {
          continue;
        }
        brief += "Dispute " + defect.id + " against test " +
                 body.value("test_id", "") + ": " + body.value("rationale", "") +
                 ". ";
      }
    }
  }

  AgentTurnRequest request = make_request(MilestonePhase::Validate, milestone_id, "qa");
  request.worktree = worktree->string();
  request.brief = std::move(brief);

  auto outcome = agent_runner_.run_turn(request);
  if (!outcome) {
    spdlog::warn("test dispute decision for {} failed: {}", milestone_id,
                 outcome.error().message);
    return TurnResult{false, "", outcome.error().message, false, false, false, false,
                      false, ""};
  }
  if (!outcome->completed) {
    return TurnResult{false, "", outcome->detail, false, false, false, false, false, ""};
  }

  // A rejection changes nothing, so a clean worktree is not an error.
  std::vector<std::string> qa_paths;
  for (const char* path : {"tests", "qa"}) {
    std::error_code ec;
    if (std::filesystem::exists(*worktree / path, ec)) {
      qa_paths.push_back(path);
    }
  }
  bool fixed = false;
  if (!qa_paths.empty()) {
    git::CommitTrailers trailers;
    trailers.run_id = config_.run_id;
    trailers.milestone_id = id;
    trailers.waivers = waivers_trailer(milestone_id);
    trailers.op_id = store::new_op_id();
    const auto committed = repository_.commit_paths(
        *worktree, qa_paths, "test(" + id + "): dispute decision", trailers);
    if (committed.has_value()) {
      fixed = true;
    } else if (committed.error().code != git::error_codes::kNoChanges) {
      return TurnResult{false, "", committed.error().message, false, false, false,
                        false, false, ""};
    }
  }

  // QA decides: a test change resolves the dispute, no change rejects it. A
  // rejection is recorded so a further dispute of the same test escalates
  // (design section 12).
  if (store_ != nullptr) {
    auto defects = store_->list_defects(config_.run_id, milestone_id,
                                        store::open_defect_statuses());
    if (defects.has_value()) {
      for (const auto& defect : *defects) {
        if (defect.status != "DISPUTED") {
          continue;
        }
        if (fixed) {
          (void)store_->set_defect_status(defect.id, "VERIFIED", defect.attempts);
          continue;
        }
        auto body = nlohmann::json::parse(defect.body_json, nullptr, false);
        if (!body.is_object() || body.value("decided", false)) {
          continue;
        }
        body["decided"] = true;
        store::DefectRecord updated = defect;
        updated.body_json = body.dump();
        (void)store_->upsert_defect(updated);
      }
    }
  }

  TurnResult result;
  result.completed = true;
  result.summary = outcome->summary;
  result.exit_criteria_met = outcome->exit_criteria_met;
  return result;
}

GateOutcome WorkspaceDriver::run_gates(std::string_view milestone_id) {
  telemetry::ScopedContext log_context(config_.run_id, std::string(milestone_id),
                                       "orchestrator");
  GateOutcome outcome;
  // A revalidating milestone is already merged: its gates run against the
  // integration head, and the review is re-run in full (design sections
  // 6.13.3, 7.4).
  const bool revalidating = revalidating_.count(std::string(milestone_id)) > 0;
  auto worktree = revalidating ? ensure_integration_worktree()
                               : ensure_worktree(milestone_id, "coder");
  if (!worktree) {
    cf::gates::GateResult result;
    result.gate = "build";
    result.passed = false;
    result.findings.push_back(
        cf::gates::GateFinding{"build", "error", worktree.error().message});
    outcome.report.results.push_back(std::move(result));
    return outcome;
  }
  auto head = repository_.head_sha(*worktree);
  if (!head) {
    cf::gates::GateResult result;
    result.gate = "build";
    result.passed = false;
    result.findings.push_back(
        cf::gates::GateFinding{"build", "error", head.error().message});
    outcome.report.results.push_back(std::move(result));
    return outcome;
  }
  outcome.commit_sha = *head;

  std::vector<std::string> changed_files;
  const auto base = base_sha_.find(milestone_id);
  if (base != base_sha_.end()) {
    auto changes = repository_.diff(base->second, *head);
    if (changes.has_value()) {
      for (const auto& change : *changes) {
        changed_files.push_back(change.path);
        FileChange review_change;
        review_change.path = change.path;
        review_change.added_lines = change.added_lines;
        review_change.removed_lines = change.removed_lines;
        review_change.qa_owned = is_qa_owned_path(change.path);
        review_change.public_interface = is_public_interface_path(change.path);
        outcome.changes_since_approved.push_back(std::move(review_change));
      }
    }
  }
  outcome.review_context.first_gate_run = gated_once_.emplace(milestone_id, true).second;
  outcome.review_context.full_review_threshold = config_.full_review_threshold;
  outcome.review_context.dependency_interface_changed = revalidating;

  auto plan = gates::build_gate_plan(config_.profile, config_.profile_language,
                                     config_.profile_build_system,
                                     config_.profile_test_framework);
  if (!plan) {
    cf::gates::GateResult result;
    result.gate = "build";
    result.passed = false;
    result.findings.push_back(
        cf::gates::GateFinding{"build", "error", plan.error().message});
    outcome.report.results.push_back(std::move(result));
    return outcome;
  }

  gates::GateRunOptions options;
  options.step_context.rules_dir = config_.rules_dir;
  options.step_context.src_dir = worktree->string();
  // A revalidation gates the integration worktree, so it needs its own build
  // directory: reusing the coder worktree's would trip CMake's source-dir
  // cache check.
  std::filesystem::path build_dir = config_.workspace_root / "build" /
                                    config_.run_id / std::string(milestone_id);
  if (revalidating) {
    build_dir /= "revalidation";
  }
  options.step_context.build_dir = build_dir.string();
  options.step_context.standard = plan_.target.standard;
  options.step_context.standard_number = gates::standard_number(plan_.target.standard);
  const auto language = config_.profile.languages.find(config_.profile_language);
  if (language != config_.profile.languages.end()) {
    options.step_context.standard_var = language->second.standard_var;
  }
  options.step_context.changed_files = changed_files;
  options.step_context.coverage_line =
      gates::coverage_percent(config_.profile.coverage.line);
  options.step_context.coverage_branch =
      gates::coverage_percent(config_.profile.coverage.branch);
  options.step_context.mutation_min_score =
      gates::ratio_string(config_.profile.mutation.min_score);
  options.working_directory = worktree->string();
  options.commit_sha = *head;
  options.step_timeout = config_.step_timeout;

  std::unique_ptr<gates::GateRunner> runner;
  std::unique_ptr<cf::sandbox::DockerSandboxExecutor> sandbox;
  std::unique_ptr<cf::sandbox::NamespaceExecutor> namespace_sandbox;
  if (config_.sandbox_namespaces) {
    // Daemon-free sandbox: the same mount layout as Docker (/work read-only
    // with writable src/include, executable /build), built from namespaces.
    sandbox::NamespaceSandboxSpec spec;
    spec.worktree = worktree->string();
    spec.writable_paths = {"src", "include"};
    spec.memory_mb = config_.gate_memory_mb;
    if (!config_.rules_dir.empty()) {
      spec.extra_readonly_mounts.push_back({config_.rules_dir, "/cf-rules"});
    }
    namespace_sandbox = std::make_unique<cf::sandbox::NamespaceExecutor>(std::move(spec));
    runner = std::make_unique<gates::GateRunner>(*namespace_sandbox);
    options.step_context.src_dir = "/work";
    options.step_context.build_dir = "/build";
    options.step_context.rules_dir =
        config_.rules_dir.empty() ? "" : "/cf-rules";
    options.working_directory = "/work";
  } else if (!config_.sandbox_image.empty()) {
    // Gates run in the sandbox: placeholders point at the container's mount
    // layout (/work read-only with writable src/include, executable /build).
    sandbox::SandboxSpec spec;
    spec.image = config_.sandbox_image;
    spec.run_id = config_.run_id;
    spec.worktree = worktree->string();
    spec.writable_paths = {"src", "include"};
    spec.cpus = config_.gate_cpus;
    spec.memory_mb = config_.gate_memory_mb;
    if (!config_.rules_dir.empty()) {
      spec.extra_mounts.push_back(
          sandbox::SandboxMount{config_.rules_dir, "/cf-rules", false});
    }
    sandbox = std::make_unique<cf::sandbox::DockerSandboxExecutor>(std::move(spec));
    runner = std::make_unique<gates::GateRunner>(*sandbox);
    options.step_context.src_dir = "/work";
    options.step_context.build_dir = "/build";
    options.step_context.rules_dir =
        config_.rules_dir.empty() ? "" : "/cf-rules";
    options.working_directory = "/work";
  } else {
    std::error_code ec;
    std::filesystem::create_directories(options.step_context.build_dir, ec);
    runner = std::make_unique<gates::GateRunner>(executor_);
  }

  outcome.report = runner->run(*plan, options);
  // A human can waive a failed gate rule at a scope (design section 10.2);
  // only fully covered gates are waived, and skipped checks never are.
  if (store_ != nullptr) {
    auto waivers = store_->list_waivers(config_.run_id, milestone_id);
    if (waivers) {
      apply_waivers(outcome.report, *waivers);
    } else {
      spdlog::warn("cannot read waivers for {}: {}", milestone_id,
                   waivers.error().message);
    }
  }
  outcome.passed = outcome.report.passed;

  if (store_ != nullptr) {
    const std::filesystem::path reports_dir =
        config_.workspace_root / "reports" / config_.run_id / std::string(milestone_id);
    std::error_code ec;
    std::filesystem::create_directories(reports_dir, ec);
    const std::filesystem::path report_path =
        reports_dir / ("gates-" + *head + ".json");
    {
      std::ofstream out(report_path, std::ios::binary);
      if (out) {
        out << outcome.report.to_json() << "\n";
      }
    }
    for (const auto& result : outcome.report.results) {
      store::GateResultRow row;
      row.run_id = config_.run_id;
      row.milestone_id = std::string(milestone_id);
      row.commit_sha = *head;
      row.gate = result.gate;
      row.passed = (result.passed || result.waived) && !result.skipped;
      row.report_ref = report_path.string();
      row.at = store::unix_now();
      (void)store_->record_gate_result(row);
    }
  }
  return outcome;
}

// Revalidation test repair (design sections 6.9, 6.13.3): an approved
// interface change can invalidate protected tests. QA owns tests/, so it
// updates them (or files a defect when the code is actually wrong) against
// the integration head.
Status WorkspaceDriver::rebase_integration_onto(std::string_view upstream) {
  telemetry::ScopedContext log_context(config_.run_id, "integration",
                                       "orchestrator");
  auto worktree = ensure_integration_worktree();
  if (!worktree) {
    return tl::make_unexpected(worktree.error());
  }
  auto rebased = repository_.rebase(*worktree, std::string(upstream));
  if (!rebased) {
    (void)repository_.abort_rebase(*worktree);
    return fail(git::error_codes::kMergeConflict,
                "cannot rebase the integration branch onto " +
                    std::string(upstream) + ": " + rebased.error().message);
  }
  return {};
}

Result<GateOutcome> WorkspaceDriver::run_integration_gates(
    std::string_view upstream) {
  telemetry::ScopedContext log_context(config_.run_id, "integration",
                                       "orchestrator");
  GateOutcome outcome;
  auto worktree = ensure_integration_worktree();
  if (!worktree) {
    return tl::make_unexpected(worktree.error());
  }
  auto head = repository_.head_sha(*worktree);
  if (!head) {
    return tl::make_unexpected(head.error());
  }
  outcome.commit_sha = *head;

  std::vector<std::string> changed_files;
  auto changes = repository_.diff(std::string(upstream), *head);
  if (changes) {
    for (const auto& change : *changes) {
      changed_files.push_back(change.path);
    }
  } else {
    spdlog::warn("integration gates: cannot diff {}..{}: {}",
                 std::string(upstream), head->substr(0, 12),
                 changes.error().message);
  }

  auto plan = gates::build_gate_plan(config_.profile, config_.profile_language,
                                     config_.profile_build_system,
                                     config_.profile_test_framework);
  if (!plan) {
    cf::gates::GateResult result;
    result.gate = "build";
    result.passed = false;
    result.findings.push_back(
        cf::gates::GateFinding{"build", "error", plan.error().message});
    outcome.report.results.push_back(std::move(result));
    return outcome;
  }

  gates::GateRunOptions options;
  options.step_context.rules_dir = config_.rules_dir;
  options.step_context.src_dir = worktree->string();
  std::filesystem::path build_dir = config_.workspace_root / "build" /
                                    config_.run_id / "integration";
  options.step_context.build_dir = build_dir.string();
  options.step_context.standard = plan_.target.standard;
  options.step_context.standard_number = gates::standard_number(plan_.target.standard);
  const auto language = config_.profile.languages.find(config_.profile_language);
  if (language != config_.profile.languages.end()) {
    options.step_context.standard_var = language->second.standard_var;
  }
  options.step_context.coverage_line =
      gates::coverage_percent(config_.profile.coverage.line);
  options.step_context.coverage_branch =
      gates::coverage_percent(config_.profile.coverage.branch);
  options.step_context.mutation_min_score =
      gates::ratio_string(config_.profile.mutation.min_score);
  options.step_context.changed_files = changed_files;
  options.working_directory = worktree->string();
  options.commit_sha = *head;
  options.step_timeout = config_.step_timeout;

  std::unique_ptr<gates::GateRunner> runner;
  std::unique_ptr<cf::sandbox::DockerSandboxExecutor> sandbox;
  std::unique_ptr<cf::sandbox::NamespaceExecutor> namespace_sandbox;
  if (config_.sandbox_namespaces) {
    sandbox::NamespaceSandboxSpec spec;
    spec.worktree = worktree->string();
    spec.writable_paths = {"src", "include"};
    spec.memory_mb = config_.gate_memory_mb;
    if (!config_.rules_dir.empty()) {
      spec.extra_readonly_mounts.push_back({config_.rules_dir, "/cf-rules"});
    }
    namespace_sandbox = std::make_unique<sandbox::NamespaceExecutor>(std::move(spec));
    runner = std::make_unique<gates::GateRunner>(*namespace_sandbox);
    options.step_context.src_dir = "/work";
    options.step_context.build_dir = "/build";
    options.step_context.rules_dir =
        config_.rules_dir.empty() ? "" : "/cf-rules";
    options.working_directory = "/work";
  } else if (!config_.sandbox_image.empty()) {
    sandbox::SandboxSpec spec;
    spec.image = config_.sandbox_image;
    spec.run_id = config_.run_id;
    spec.worktree = worktree->string();
    spec.writable_paths = {"src", "include"};
    spec.cpus = config_.gate_cpus;
    spec.memory_mb = config_.gate_memory_mb;
    if (!config_.rules_dir.empty()) {
      spec.extra_mounts.push_back(
          sandbox::SandboxMount{config_.rules_dir, "/cf-rules", false});
    }
    sandbox = std::make_unique<cf::sandbox::DockerSandboxExecutor>(std::move(spec));
    runner = std::make_unique<gates::GateRunner>(*sandbox);
    options.step_context.src_dir = "/work";
    options.step_context.build_dir = "/build";
    options.step_context.rules_dir =
        config_.rules_dir.empty() ? "" : "/cf-rules";
    options.working_directory = "/work";
  } else {
    std::error_code ec;
    std::filesystem::create_directories(options.step_context.build_dir, ec);
    runner = std::make_unique<gates::GateRunner>(executor_);
  }

  outcome.report = runner->run(*plan, options);
  if (store_ != nullptr) {
    auto waivers = store_->list_waivers(config_.run_id);
    if (waivers) {
      apply_waivers(outcome.report, *waivers);
    }
  }
  outcome.passed = outcome.report.passed;

  if (store_ != nullptr) {
    const std::filesystem::path reports_dir =
        config_.workspace_root / "reports" / config_.run_id / "integration";
    std::error_code ec;
    std::filesystem::create_directories(reports_dir, ec);
    const std::filesystem::path report_path =
        reports_dir / ("gates-" + *head + ".json");
    {
      std::ofstream report_out(report_path, std::ios::binary);
      if (report_out) {
        report_out << outcome.report.to_json() << "\n";
      }
    }
    for (const auto& result : outcome.report.results) {
      store::GateResultRow row;
      row.run_id = config_.run_id;
      row.milestone_id = "integration";
      row.commit_sha = *head;
      row.gate = result.gate;
      row.passed = (result.passed || result.waived) && !result.skipped;
      row.report_ref = report_path.string();
      row.at = store::unix_now();
      (void)store_->record_gate_result(row);
    }
  }
  return outcome;
}

TurnResult WorkspaceDriver::run_test_repair(std::string_view milestone_id) {
  telemetry::ScopedContext log_context(config_.run_id, std::string(milestone_id),
                                       "qa");
  const std::string id(milestone_id);
  auto worktree = ensure_integration_worktree();
  if (!worktree) {
    return TurnResult{false, "", worktree.error().message, false, false, false, false,
                      false, ""};
  }
  record_checkpoint(milestone_id);

  std::string brief =
      "Revalidation test repair for milestone " + id +
      ". An approved interface change was merged and the gates now fail. The "
      "tests under tests/ are QA-owned: if a test asserts the old interface, "
      "update it to the new contract; if the code itself is wrong, file a "
      "defect instead. Finish by calling the submission tool.";
  if (store_ != nullptr) {
    const std::string failures =
        failing_gate_summary(*store_, config_.run_id, id);
    if (!failures.empty()) {
      brief += " Failing gates: " + failures;
    }
  }

  AgentTurnRequest request = make_request(MilestonePhase::Validate, milestone_id, "qa");
  request.worktree = worktree->string();
  request.brief = std::move(brief);
  auto outcome = agent_runner_.run_turn(request);
  if (!outcome) {
    spdlog::warn("test repair for {} failed: {}", milestone_id,
                 outcome.error().message);
    return TurnResult{false, "", outcome.error().message, false, false, false, false,
                      false, ""};
  }
  if (!outcome->completed) {
    return TurnResult{false, "", outcome->detail, false, false, false, false, false, ""};
  }

  // Commit the repaired tests on the integration branch; a QA turn that only
  // filed a defect changes nothing.
  std::vector<std::string> qa_paths;
  for (const char* path : {"tests", "qa"}) {
    std::error_code ec;
    if (std::filesystem::exists(*worktree / path, ec)) {
      qa_paths.push_back(path);
    }
  }
  if (!qa_paths.empty()) {
    git::CommitTrailers trailers;
    trailers.run_id = config_.run_id;
    trailers.milestone_id = id;
    trailers.waivers = waivers_trailer(milestone_id);
    trailers.op_id = store::new_op_id();
    const auto committed = repository_.commit_paths(
        *worktree, qa_paths, "test(" + id + "): revalidation repair", trailers);
    if (!committed && committed.error().code != git::error_codes::kNoChanges) {
      return TurnResult{false, "", committed.error().message, false, false, false,
                        false, false, ""};
    }
  }

  TurnResult result;
  result.completed = true;
  result.summary = outcome->summary;
  result.exit_criteria_met = outcome->exit_criteria_met;
  return result;
}

std::vector<std::string> WorkspaceDriver::integration_interfaces(
    std::string_view milestone_id) {
  const auto it = changed_interfaces_.find(std::string(milestone_id));
  return it == changed_interfaces_.end() ? std::vector<std::string>{} : it->second;
}

void WorkspaceDriver::mark_revalidating(std::string_view milestone_id) {
  revalidating_.insert(std::string(milestone_id));
}

SystemValidationResult WorkspaceDriver::validate_system() {
  telemetry::ScopedContext log_context(config_.run_id, "", "qa");
  AgentTurnRequest request;
  request.phase = MilestonePhase::Validate;
  request.run_id = config_.run_id;
  request.role = "qa";
  request.worktree = integration_worktree().string();
  request.brief = "Run the full system validation suite on the merged result";
  auto outcome = agent_runner_.run_system_validation(request);
  if (!outcome) {
    return SystemValidationResult{false, false, outcome.error().message};
  }
  return SystemValidationResult{outcome->completed, outcome->exit_criteria_met,
                                outcome->detail};
}

}  // namespace cf::orch
