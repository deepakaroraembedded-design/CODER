#include "cf/orch/driver.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include "cf/sandbox/docker.hpp"
#include "cf/sandbox/namespaces.hpp"

namespace cf::orch {
namespace {

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
  trailers.op_id = op_id;
  auto sha = repository_.commit_paths(*worktree, existing, message, trailers);
  if (!sha) {
    return tl::make_unexpected(sha.error());
  }
  return {};
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
  if (const plan::MilestoneSpec* spec = plan_.find_milestone(milestone_id)) {
    request.requirement_ids = spec->requirements;
    switch (phase) {
      case MilestonePhase::AuthorTests:
        request.brief = "Author the test plan and tests for milestone " +
                        std::string(milestone_id) + " (" + spec->title + ")";
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
    if (!spec->acceptance.empty()) {
      request.brief += ". Acceptance criteria: " + join(spec->acceptance);
    }
    if (!plan_.forbidden.empty()) {
      request.brief += ". Forbidden: " + join(plan_.forbidden);
    }
  }
  return request;
}

TurnResult WorkspaceDriver::run(MilestonePhase phase, std::string_view milestone_id) {
  if (phase == MilestonePhase::Integrate) {
    // The integration worktree lives under wt/<run>/integration.
    const std::filesystem::path path = integration_worktree();
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
      auto created = repository_.create_worktree(path, config_.integration_branch,
                                                 config_.base_branch);
      if (!created) {
        return TurnResult{false, "", created.error().message, false, false, false, false,
                          false};
      }
    }
    auto before = repository_.head_sha(path);
    if (!before) {
      return TurnResult{false, "", before.error().message, false, false, false, false,
                        false};
    }
    const std::string branch = "cf-local/" + config_.run_id + "/" +
                               std::string(milestone_id) + "/coder";
    git::CommitTrailers trailers;
    trailers.run_id = config_.run_id;
    trailers.milestone_id = std::string(milestone_id);
    trailers.op_id = store::new_op_id();
    trailers.squashed_from = branch;
    auto merged = repository_.merge_squash(path, branch,
                                           "feat(" + std::string(milestone_id) + "): " +
                                               std::string(milestone_id),
                                           trailers);
    if (!merged) {
      if (merged.error().code == git::error_codes::kMergeConflict) {
        (void)repository_.abort_merge(path);
        TurnResult conflict;
        conflict.completed = true;
        conflict.merged = false;
        conflict.conflicts = true;
        conflict.detail = merged.error().message;
        return conflict;
      }
      return TurnResult{false, "", merged.error().message, false, false, false, false,
                        false};
    }
    auto changes = repository_.diff(*before, *merged);
    bool interface_changed = false;
    if (changes.has_value()) {
      interface_changed = std::any_of(changes->begin(), changes->end(),
                                      [](const git::FileChange& change) {
                                        return is_public_interface_path(change.path);
                                      });
    }
    TurnResult result;
    result.completed = true;
    result.merged = true;
    result.interface_changed = interface_changed;
    result.summary = "merged " + branch;
    return result;
  }

  const std::string role =
      phase == MilestonePhase::Review ? "reviewer"
                                      : (phase == MilestonePhase::AuthorTests ||
                                                 phase == MilestonePhase::Validate
                                             ? "qa"
                                             : "coder");
  auto worktree = ensure_worktree(milestone_id, role);
  if (!worktree) {
    return TurnResult{false, "", worktree.error().message, false, false, false, false,
                      false};
  }

  const AgentTurnRequest request = make_request(phase, milestone_id, role);
  auto outcome = agent_runner_.run_turn(request);
  if (!outcome) {
    spdlog::warn("{} turn for {} failed: {}", role, milestone_id,
                 outcome.error().message);
    return TurnResult{false, "", outcome.error().message, false, false, false, false,
                      false};
  }
  if (!outcome->completed) {
    return TurnResult{false, "", outcome->detail, false, false, false, false, false};
  }

  switch (phase) {
    case MilestonePhase::AuthorTests: {
      auto stored = snapshot(milestone_id, role, {"tests", "qa"},
                             "test(" + std::string(milestone_id) + "): " +
                                 outcome->summary,
                             store::new_op_id());
      if (!stored) {
        return TurnResult{false, "", stored.error().message, false, false, false, false,
                          false};
      }
      TurnResult result;
      result.completed = true;
      result.summary = outcome->summary;
      return result;
    }
    case MilestonePhase::Code:
    case MilestonePhase::Fix: {
      const std::string prefix = phase == MilestonePhase::Code ? "feat" : "fix";
      auto stored = snapshot(milestone_id, role, {"src", "include"},
                             prefix + "(" + std::string(milestone_id) + "): " +
                                 outcome->summary,
                             store::new_op_id());
      if (!stored) {
        return TurnResult{false, "", stored.error().message, false, false, false, false,
                          false};
      }
      TurnResult result;
      result.completed = true;
      result.summary = outcome->summary;
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
  return TurnResult{false, "", "unreachable phase", false, false, false, false, false};
}

GateOutcome WorkspaceDriver::run_gates(std::string_view milestone_id) {
  GateOutcome outcome;
  auto worktree = ensure_worktree(milestone_id, "coder");
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
  outcome.review_context.full_review_threshold = 400;

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
  options.step_context.src_dir = worktree->string();
  options.step_context.build_dir =
      (config_.workspace_root / "build" / config_.run_id / std::string(milestone_id))
          .string();
  options.step_context.standard = plan_.target.standard;
  options.step_context.standard_number = gates::standard_number(plan_.target.standard);
  const auto language = config_.profile.languages.find(config_.profile_language);
  if (language != config_.profile.languages.end()) {
    options.step_context.standard_var = language->second.standard_var;
  }
  options.step_context.changed_files = changed_files;
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
    namespace_sandbox = std::make_unique<cf::sandbox::NamespaceExecutor>(std::move(spec));
    runner = std::make_unique<gates::GateRunner>(*namespace_sandbox);
    options.step_context.src_dir = "/work";
    options.step_context.build_dir = "/build";
    options.working_directory = "/work";
  } else if (!config_.sandbox_image.empty()) {
    // Gates run in the sandbox: placeholders point at the container's mount
    // layout (/work read-only with writable src/include, executable /build).
    sandbox::SandboxSpec spec;
    spec.image = config_.sandbox_image;
    spec.worktree = worktree->string();
    spec.writable_paths = {"src", "include"};
    sandbox = std::make_unique<cf::sandbox::DockerSandboxExecutor>(std::move(spec));
    runner = std::make_unique<gates::GateRunner>(*sandbox);
    options.step_context.src_dir = "/work";
    options.step_context.build_dir = "/build";
    options.working_directory = "/work";
  } else {
    std::error_code ec;
    std::filesystem::create_directories(options.step_context.build_dir, ec);
    runner = std::make_unique<gates::GateRunner>(executor_);
  }

  outcome.report = runner->run(*plan, options);
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
      row.passed = result.passed && !result.skipped;
      row.report_ref = report_path.string();
      row.at = store::unix_now();
      (void)store_->record_gate_result(row);
    }
  }
  return outcome;
}

SystemValidationResult WorkspaceDriver::validate_system() {
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
