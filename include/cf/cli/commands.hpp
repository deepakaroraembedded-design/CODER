// cf-cli: command implementations (design section 9.2).
//
// Thin, honest layers over the built subsystems: plan validation and run
// initialization, status, recovery (cf resume), escalation bundles and the
// final report. Commands return a process exit code and write to the given
// streams so they are unit-testable.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/process.hpp"
#include "cf/llm/client.hpp"
#include "cf/orch/refiner.hpp"
#include "cf/orch/session_agent_runner.hpp"

namespace cf::cli {

// Workspace layout: <workspace>/runs/<run_id>/run.db, plan.yaml, escalations/
// and <workspace>/wt/... for worktrees.
std::filesystem::path run_directory(const std::filesystem::path& workspace,
                                    const std::string& run_id);

// Validates the plan against the profile registry (FR-18) and prints a JSON
// summary. Exit 0 on success, 1 on an invalid plan, 2 on an environment error.
int check_command(const std::filesystem::path& plan_path,
                  const std::filesystem::path& profiles_dir, std::ostream& out,
                  std::ostream& err);

// Creates the run: validates the plan, creates the run store, copies the plan
// into the run directory and registers the milestones in Pending. An empty
// run_id is generated. Exit 0 on success, 1 when the run already exists,
// 2 on an environment error.
int run_command(const std::filesystem::path& plan_path,
                const std::filesystem::path& profiles_dir,
                const std::filesystem::path& workspace, const std::string& run_id,
                std::ostream& out, std::ostream& err);

// Prints the run state: milestones, operations, locks, lease, cost, recent
// transitions, gate results and defects. `json` selects machine output.
int status_command(const std::filesystem::path& workspace, const std::string& run_id,
                   bool json, std::ostream& out, std::ostream& err);

// cf resume: takes the lease, releases stale locks, reconciles in-flight
// operations and resets worktrees to their checkpoints when a repository is
// given. Prints the recovery report.
int resume_command(const std::filesystem::path& workspace, const std::string& run_id,
                   const std::string& holder, const std::filesystem::path& repository_path,
                   std::ostream& out, std::ostream& err);

// Lists the escalation bundles of a run as JSON.
int escalations_command(const std::filesystem::path& workspace,
                        const std::string& run_id, std::ostream& out, std::ostream& err);

// Writes report.json and report.md: milestone/gate history, requirement
// traceability, defects, cost and escalations.
int report_command(const std::filesystem::path& workspace, const std::string& run_id,
                   const std::filesystem::path& out_dir, std::ostream& out,
                   std::ostream& err);

// cf approve --stage design: moves a run from AwaitingDesignApproval to
// Executing (the human approval gate, design section 7.1).
int approve_command(const std::filesystem::path& workspace, const std::string& run_id,
                    const std::string& stage, std::ostream& out, std::ostream& err);

// cf approve --stage design --request-changes --reason <text>: moves a run
// from AwaitingDesignApproval back to Refining, archives the rejected design
// package and records the feedback for the next refinement (design section
// 7.1).
int request_design_changes_command(const std::filesystem::path& workspace,
                                   const std::string& run_id,
                                   const std::string& reason, std::ostream& out,
                                   std::ostream& err);

// cf approve --refuse-waiver <id> --reason <text>: refuses an agent waiver
// request (design section 12.1); the run can then continue or escalate.
int refuse_waiver_request_command(const std::filesystem::path& workspace,
                                  const std::string& run_id,
                                  const std::string& request_id,
                                  const std::string& reason, std::ostream& out,
                                  std::ostream& err);

// cf approve --waiver <rule> --scope <file:line>: records a human waiver of
// a gate rule (design section 10.2). Agents cannot grant waivers; the record
// carries the approver and a written justification and is listed in the run
// report and the pull request body (design section 10.4.8).
struct WaiverOptions {
  std::string rule;
  std::string scope;
  std::string justification;
  std::string approved_by;
  std::string milestone_id;  // empty: applies to the whole run
};
int approve_waiver_command(const std::filesystem::path& workspace,
                           const std::string& run_id,
                           const WaiverOptions& options, std::ostream& out,
                           std::ostream& err);

// cf replay: re-sends an archived LLM request to the same or a different
// model (design sections 6.11, 14).
struct ReplayOptions {
  std::filesystem::path workspace;
  std::string run_id;
  std::string call_id;
  std::string model;                  // empty: the archived model
  std::string api_key_env = "FIREWORKS_API_KEY";
  std::string base_url;               // empty: the archived base URL
  cf::llm::HttpTransport* transport = nullptr;  // tests inject one
};
int replay_command(const ReplayOptions& options, std::ostream& out,
                   std::ostream& err);

// cf remind: posts the §12.1 waiting-state reminders on the run's pull
// request (design approval after 2 and 4 business days, escalation after 1
// and 2). Already-posted reminders are recognised by a hidden marker.
struct ReminderOptions {
  std::filesystem::path workspace;
  std::string run_id;  // empty with all=true: sweep every run
  bool all = false;
  std::filesystem::path repository_path;  // local checkout (for CODEOWNERS fallback)
  std::string github_owner;
  std::string github_repo;
  std::string github_api_base = "https://api.github.com";
  std::string github_app_id;
  std::string github_installation_id;
  std::string github_private_key_file;
  std::string github_token;
  cf::llm::HttpTransport* github_transport = nullptr;
};
int reminder_command(const ReminderOptions& options, std::ostream& out,
                     std::ostream& err);

// cf rulesets: writes the GitHub ruleset payloads a human with admin rights
// applies (design section 10.4.7); the CF bot has no admin permission.
struct RulesetCommandOptions {
  std::filesystem::path out_dir;  // empty: print the payloads
  std::vector<std::string> required_checks;
};
int rulesets_command(const RulesetCommandOptions& options, std::ostream& out,
                     std::ostream& err);

// cf cleanup: deletes the run's cf/ branches once its pull request has been
// merged or closed and the retention window has passed (design section
// 10.4.9). Idempotent: a missing branch is already clean.
struct CleanupOptions {
  std::filesystem::path workspace;
  std::string run_id;  // empty with all=true: sweep every run
  bool all = false;
  std::filesystem::path repository_path;  // local checkout with the remote
  std::string github_owner;
  std::string github_repo;
  std::string github_api_base = "https://api.github.com";
  std::string github_app_id;
  std::string github_installation_id;
  std::string github_private_key_file;
  std::string github_token;
  std::string github_remote = "origin";
  std::uint32_t retention_days = 30;
  // Tests inject a transport; production uses cpr.
  cf::llm::HttpTransport* github_transport = nullptr;
};
int cleanup_command(const CleanupOptions& options, std::ostream& out,
                    std::ostream& err);

// cf abort: a human aborts an escalated run (design section 7.1) and the
// designs it published are withdrawn from RAG (design section 6.2 step 7).
int abort_command(const std::filesystem::path& workspace, const std::string& run_id,
                  const std::string& reason, std::ostream& out, std::ostream& err);

struct IngestOptions {
  std::filesystem::path docs_dir;  // Markdown with front matter, plus PDF/DOCX with sidecars
  std::filesystem::path out_dir;   // optional: normalized copies for cf-mcp --docs
  std::filesystem::path db_path;   // optional: run store for doc_versions
  // Used to run pdftotext/pandoc for PDF/DOCX inputs; tests inject one.
  cf::process::ProcessExecutor* executor = nullptr;
};

// cf ingest: validates company documents, holds anything that looks like an
// injected instruction, writes normalized copies and records document
// versions (design sections 6.3.1, 8.4).
int ingest_command(const IngestOptions& options, std::ostream& out, std::ostream& err);

struct ExecuteOptions {
  std::filesystem::path workspace;
  std::string run_id;
  std::filesystem::path profiles_dir = "profiles";
  std::filesystem::path repository_path;  // the target repository (required)
  std::string base_branch = "main";
  std::string opencode_binary = "opencode";
  std::string mcp_binary = "cf-mcp";
  std::map<std::string, std::string, std::less<>> role_models;
  // Extra opencode model options per role (e.g. temperature), typically from
  // cf.yaml (design section 15, models.roles).
  std::map<std::string, nlohmann::json, std::less<>> role_options;
  std::string sandbox_image;  // empty: gates run through the local executor
  std::string docs_dir;
  // Design refinement (design section 6.2). Unless --skip-refinement is set,
  // the first execution refines the plan, writes the design package and stops
  // at AwaitingDesignApproval; `cf approve --stage design` releases it.
  bool skip_refinement = false;
  std::string design_model;  // planner model; empty: the coder role's model
  std::string provider_base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key_env = "FIREWORKS_API_KEY";
  // Pattern catalog for the refiner prompt and the role MCP servers (design
  // section 6.2); a missing directory is ignored.
  std::string patterns_dir = "catalog/patterns";
  // Tests inject the planner; production builds a Fireworks client.
  cf::orch::DesignModel* design_model_override = nullptr;

  // GitHub flow (design section 10.4.4); enabled when owner and repo are set.
  std::string github_owner;
  std::string github_repo;
  std::string github_api_base = "https://api.github.com";
  // GitHub App identity (design section 10.4.2); the key file is read into
  // memory and never written anywhere.
  std::string github_app_id;
  std::string github_installation_id;
  std::string github_private_key_file;
  // SSH private key that signs every commit (design section 10.4.2); empty
  // leaves commits unsigned (local runs without a key).
  std::string github_signing_key_file;
  std::string github_token;  // fallback: a static token
  // Pre-push policy (design section 10.4.6); unset fields keep the defaults.
  std::optional<std::string> github_branch_prefix;
  std::optional<std::vector<std::string>> github_protected_paths;
  std::optional<std::uint64_t> github_max_file_mb;
  // When the flow pushes (design section 10.4.4): milestone_done, run_done,
  // escalation. Unset keeps all three.
  std::optional<std::vector<std::string>> github_push_on;
  std::optional<std::uint32_t> branch_retention_days;
  std::optional<bool> github_pr_draft_until_done;
  std::optional<std::vector<std::string>> github_pr_labels;
  std::optional<bool> github_pr_request_codeowners;
  // Tests inject a transport; production uses cpr.
  cf::llm::HttpTransport* github_transport = nullptr;

  // cf.yaml (design section 15), applied below any flag: budget defaults (a
  // plan's budget block still wins), loop limits, the review threshold and
  // gate sandbox resources.
  std::optional<double> budget_usd;
  std::optional<std::uint64_t> budget_tokens;
  std::optional<std::uint32_t> max_iterations_per_milestone;
  std::optional<std::uint32_t> max_attempts_per_defect;
  std::optional<double> budget_wall_clock_hours;
  // Waiting-state deadlines in business days (design section 12.1); a plan's
  // approvals block overrides these defaults.
  std::optional<std::uint32_t> design_approval_business_days;
  std::optional<std::uint32_t> escalation_business_days;
  std::optional<std::uint32_t> waiver_business_days;
  // Retrieval settings (design sections 6.3, 15).
  std::optional<std::string> rag_index;
  std::optional<std::uint32_t> rag_top_k;
  std::optional<double> rag_bm25_weight;
  std::optional<double> rag_vector_weight;
  std::optional<std::vector<std::string>> rag_precedence;
  // Embeddings for hybrid retrieval (design sections 6.3, 15); empty leaves
  // the retriever BM25-only.
  std::string embeddings_model;
  std::optional<std::uint32_t> embeddings_dim;
  std::string embeddings_base_url;
  std::string embeddings_api_key_env = "FIREWORKS_API_KEY";
  // Offline reference documentation (design section 6.3.1); unset leaves it
  // off.
  std::optional<std::string> reference_docs_manifest;
  std::optional<std::string> reference_docs_version_source;
  std::optional<std::string> reference_docs_on_missing_version;
  std::optional<std::uint32_t> review_diff_threshold_lines;
  std::optional<std::vector<std::string>> review_blocking_severities;
  std::optional<double> sandbox_cpus;
  std::optional<double> sandbox_memory_gb;
  std::optional<std::uint32_t> gate_timeout_min;
  // Pinned toolchain profile id (gates.profile); empty or "auto" resolves
  // from the plan's target block.
  std::optional<std::string> profile_id;
  // Stage-3 gate overrides (design section 15); the profile defaults win
  // when unset.
  std::optional<double> coverage_line;
  std::optional<double> coverage_branch;
  std::optional<bool> mutation_enabled;
  std::optional<double> mutation_min_score;
};

// cf run --execute: drives the run with the production opencode factory, or
// with an injected factory in tests.
int execute_command(const ExecuteOptions& options, std::ostream& out, std::ostream& err,
                    cf::orch::SessionDriverFactory session_driver_factory = {});

}  // namespace cf::cli
