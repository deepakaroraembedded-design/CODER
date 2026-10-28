// cf: the Coder Framework command line (design section 9.2).
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "cf/cli/commands.hpp"
#include "cf/cli/config.hpp"
#include "cf/telemetry/logging.hpp"

int main(int argc, char** argv) {
  cf::telemetry::configure_json_logging();
  CLI::App app{"cf: Coder Framework (multi-agent, guardrail-driven code generation)"};

  std::string plan_path;
  std::string profiles_dir = "profiles";
  std::string workspace = "cf-work";
  std::string run_id;
  std::string holder = "cf-cli";
  std::string repository_path;
  std::string out_dir;
  bool json = false;

  auto* check = app.add_subcommand("check", "Validate a plan against the toolchain profiles");
  check->add_option("--plan", plan_path, "Plan file (YAML or Markdown with front matter)")
      ->required();
  check->add_option("--profiles", profiles_dir, "Toolchain profiles directory");

  auto* run = app.add_subcommand("run", "Validate a plan and create a run");
  run->add_option("--plan", plan_path, "Plan file")->required();
  run->add_option("--profiles", profiles_dir, "Toolchain profiles directory");
  run->add_option("--workspace", workspace, "Workspace directory");
  run->add_option("--run-id", run_id, "Run id (generated when omitted)");
  bool execute = false;
  std::string opencode_binary = "opencode";
  std::string mcp_binary = "cf-mcp";
  std::string sandbox_image;
  bool agent_sandbox_namespaces = false;
  std::string ns_agent_binary = "cf-ns-agent";
  std::string docs_dir;
  bool skip_refinement = false;
  std::vector<std::string> model_overrides;
  std::string github_owner;
  std::string github_repo;
  std::string github_app_id;
  std::string github_installation_id;
  std::string github_key_file;
  std::string github_signing_key_file;
  std::string github_token;
  std::string github_remote = "origin";
  std::uint32_t retention_days = 30;
  std::string replay_call;
  std::string replay_model;
  std::string replay_base_url;
  std::string replay_api_key_env = "FIREWORKS_API_KEY";
  run->add_flag("--execute", execute, "Execute the run after creating it");
  run->add_option("--repo", repository_path, "Target repository (with --execute)");
  run->add_option("--opencode", opencode_binary, "opencode binary");
  run->add_option("--mcp", mcp_binary, "cf-mcp binary");
  run->add_option("--agent-sandbox-namespaces", agent_sandbox_namespaces,
                  "Run opencode servers in the daemon-free namespace sandbox");
  run->add_option("--ns-agent", ns_agent_binary, "cf-ns-agent binary");
  run->add_option("--sandbox-image", sandbox_image,
                  "Run gates in this Docker image (design section 6.8)");
  run->add_option("--docs", docs_dir, "Documentation directory for the RAG index");
  run->add_flag("--skip-refinement", skip_refinement,
                "Skip the design refiner and the approval gate (design section "
                "6.2)");
  std::string design_model;
  run->add_option("--design-model", design_model,
                  "Planner model for the design refiner (default: the coder "
                  "role's model)");
  std::string patterns_dir = "catalog/patterns";
  run->add_option("--patterns", patterns_dir,
                  "Pattern catalog directory for the refiner and the agents");
  run->add_option("--model", model_overrides,
                  "Per-role model override, e.g. --model coder=accounts/...");
  run->add_option("--github-owner", github_owner, "GitHub owner for the PR flow");
  run->add_option("--github-repo", github_repo, "GitHub repository for the PR flow");
  run->add_option("--github-app-id", github_app_id, "GitHub App id");
  run->add_option("--github-installation-id", github_installation_id,
                  "GitHub App installation id");
  run->add_option("--github-key-file", github_key_file,
                  "GitHub App private key PEM file (read into memory)");
  run->add_option("--github-signing-key-file", github_signing_key_file,
                  "SSH private key that signs every CF commit");
  run->add_option("--github-token", github_token, "Static GitHub token");
  std::string config_path;
  run->add_option("--config", config_path,
                  "cf.yaml configuration file (design section 15); default: "
                  "./cf.yaml when it exists");

  auto* approve = app.add_subcommand(
      "approve", "Approve a run stage (design gate) or record a human waiver");
  std::string stage = "design";
  approve->add_option("--run", run_id, "Run id")->required();
  approve->add_option("--workspace", workspace, "Workspace directory");
  approve->add_option("--stage", stage, "Approval stage (design)");
  std::string waiver_rule;
  std::string waiver_scope;
  std::string waiver_justification;
  const char* user_env = std::getenv("USER");
  if (user_env == nullptr) {
    user_env = std::getenv("LOGNAME");
  }
  std::string waiver_approved_by = user_env == nullptr ? "" : user_env;
  std::string waiver_milestone;
  approve->add_option("--waiver", waiver_rule,
                      "Record a human waiver of a gate rule (design section "
                      "10.2); requires --scope and --justification");
  approve->add_option("--scope", waiver_scope,
                      "What the waiver covers, e.g. src/foo.cpp:42");
  approve->add_option("--justification", waiver_justification,
                      "Written reason for the waiver (recorded in the report)");
  approve->add_option("--approved-by", waiver_approved_by,
                      "Person approving the waiver (default: USER)");
  approve->add_option("--milestone", waiver_milestone,
                      "Milestone the waiver applies to (default: the whole run)");
  bool request_changes = false;
  std::string design_feedback;
  approve->add_flag("--request-changes", request_changes,
                    "Send the design back to Refining with feedback");
  approve->add_option("--reason", design_feedback,
                      "Why the design needs changes or why a waiver is refused");
  std::string refuse_waiver_id;
  approve->add_option("--refuse-waiver", refuse_waiver_id,
                      "Refuse a pending waiver request by id");

  auto* abort = app.add_subcommand("abort", "Abort an escalated run (human decision)");
  std::string abort_reason;
  abort->add_option("--run", run_id, "Run id")->required();
  abort->add_option("--workspace", workspace, "Workspace directory");
  abort->add_option("--reason", abort_reason, "Why the run is being aborted");

  auto* ingest = app.add_subcommand("ingest", "Ingest company documents into the RAG index");
  std::string docs_input;
  std::string ingest_out;
  std::string ingest_db;
  ingest->add_option("--docs", docs_input, "Documents directory (Markdown + front matter)")
      ->required();
  ingest->add_option("--out", ingest_out,
                     "Output directory for normalized copies (cf-mcp --docs)");
  ingest->add_option("--db", ingest_db, "Run store to record document versions");

  auto* status = app.add_subcommand("status", "Show a run's state");
  status->add_option("--run", run_id, "Run id")->required();
  status->add_option("--workspace", workspace, "Workspace directory");
  status->add_option("--json", json, "Machine-readable output");

  auto* resume = app.add_subcommand("resume", "Recover a run after a crash");
  resume->add_option("--run", run_id, "Run id")->required();
  resume->add_option("--workspace", workspace, "Workspace directory");
  resume->add_option("--holder", holder, "This orchestrator's identity");
  resume->add_option("--repo", repository_path,
                     "Target repository for the worktree reset (optional)");

  auto* escalations = app.add_subcommand("escalations", "List the escalation bundles");
  escalations->add_option("--run", run_id, "Run id")->required();
  escalations->add_option("--workspace", workspace, "Workspace directory");

  auto* report = app.add_subcommand("report", "Write the final run report");
  report->add_option("--run", run_id, "Run id")->required();
  report->add_option("--workspace", workspace, "Workspace directory");
  report->add_option("--out", out_dir, "Output directory")->required();

  auto* cleanup = app.add_subcommand(
      "cleanup", "Delete a merged/closed run's cf/ branches after retention");
  bool cleanup_all = false;
  cleanup->add_option("--run", run_id, "Run id");
  cleanup->add_flag("--all", cleanup_all, "Sweep every run in the workspace");
  cleanup->add_option("--workspace", workspace, "Workspace directory");
  cleanup->add_option("--repo", repository_path,
                      "Local checkout whose remote has the cf/ branches");
  cleanup->add_option("--config", config_path,
                      "cf.yaml configuration file (default: ./cf.yaml)");
  cleanup->add_option("--github-owner", github_owner, "GitHub owner");
  cleanup->add_option("--github-repo", github_repo, "GitHub repository");
  cleanup->add_option("--github-app-id", github_app_id, "GitHub App id");
  cleanup->add_option("--github-installation-id", github_installation_id,
                      "GitHub App installation id");
  cleanup->add_option("--github-key-file", github_key_file,
                      "GitHub App private key PEM file");
  cleanup->add_option("--github-token", github_token, "Static GitHub token");
  cleanup->add_option("--github-remote", github_remote,
                      "Git remote name (default: origin)");
  cleanup->add_option("--retention-days", retention_days,
                      "Days to keep the branches after the PR closes");

  auto* remind = app.add_subcommand(
      "remind", "Post due waiting-state reminders on a run's pull request");
  bool remind_all = false;
  remind->add_option("--run", run_id, "Run id");
  remind->add_flag("--all", remind_all, "Sweep every run in the workspace");
  remind->add_option("--workspace", workspace, "Workspace directory");
  remind->add_option("--config", config_path,
                     "cf.yaml configuration file (default: ./cf.yaml)");
  remind->add_option("--github-owner", github_owner, "GitHub owner");
  remind->add_option("--github-repo", github_repo, "GitHub repository");
  remind->add_option("--github-app-id", github_app_id, "GitHub App id");
  remind->add_option("--github-installation-id", github_installation_id,
                     "GitHub App installation id");
  remind->add_option("--github-key-file", github_key_file,
                     "GitHub App private key PEM file");
  remind->add_option("--github-token", github_token, "Static GitHub token");

  auto* rulesets = app.add_subcommand(
      "rulesets", "Write the GitHub ruleset payloads for a human to apply");
  std::string rulesets_out;
  std::vector<std::string> ruleset_checks;
  rulesets->add_option("--out", rulesets_out,
                       "Output directory (default: print JSON)");
  rulesets->add_option("--required-check", ruleset_checks,
                       "Required CI status-check context (repeatable)");

  auto* replay = app.add_subcommand(
      "replay", "Re-send an archived LLM request to a model");
  replay->add_option("--run", run_id, "Run id")->required();
  replay->add_option("--call", replay_call,
                     "Archived call id (call_...)")->required();
  replay->add_option("--workspace", workspace, "Workspace directory");
  replay->add_option("--model", replay_model,
                     "Model override (default: the archived model)");
  replay->add_option("--base-url", replay_base_url,
                     "Provider base URL override");
  replay->add_option("--api-key-env", replay_api_key_env,
                     "Environment variable holding the provider key");

  CLI11_PARSE(app, argc, argv);

  if (*check) {
    return cf::cli::check_command(plan_path, profiles_dir, std::cout, std::cerr);
  }
  if (*run) {
    // cf.yaml (design section 15): explicit --config, else ./cf.yaml when it
    // exists. Flags override the file; the file overrides built-in defaults.
    cf::cli::FrameworkConfig file_config;
    std::string config_file = config_path;
    if (config_file.empty() && std::filesystem::exists("cf.yaml")) {
      config_file = "cf.yaml";
    }
    if (!config_file.empty()) {
      auto loaded = cf::cli::load_config(config_file);
      if (!loaded) {
        std::cerr << loaded.error().code << ": " << loaded.error().message << "\n";
        return 2;
      }
      file_config = std::move(*loaded);
      if (run->count("--workspace") == 0 && file_config.run.workspace.has_value()) {
        workspace = *file_config.run.workspace;
      }
      if (run->count("--profiles") == 0 && file_config.gates.profiles_dir.has_value()) {
        profiles_dir = *file_config.gates.profiles_dir;
      }
    }

    const int created = cf::cli::run_command(plan_path, profiles_dir, workspace, run_id,
                                             std::cout, std::cerr);
    if (created != 0 || !execute) {
      return created;
    }
    cf::cli::ExecuteOptions options;
    // Pinned defaults from third_party/VERSIONS; cf.yaml overrides them
    // (apply_config), and --model overrides both (below).
    options.role_models = {
        {"coder", "accounts/fireworks/models/deepseek-v4p1-flash"},
        {"reviewer", "accounts/fireworks/models/kimi-k3"},
        {"qa", "accounts/fireworks/models/glm-5p3"}};
    auto applied = cf::cli::apply_config(options, file_config);
    if (!applied) {
      std::cerr << applied.error().code << ": " << applied.error().message << "\n";
      return 2;
    }
    options.workspace = workspace;      // already config-aware above
    options.profiles_dir = profiles_dir;
    options.run_id = run_id;
    if (run->count("--repo") > 0 || options.repository_path.empty()) {
      options.repository_path = repository_path;  // flag wins over run.repo
    }
    options.opencode_binary = opencode_binary;
    options.mcp_binary = mcp_binary;
    options.sandbox_image = sandbox_image;
    options.agent_sandbox_namespaces = agent_sandbox_namespaces;
    options.ns_agent_binary = ns_agent_binary;
    options.docs_dir = docs_dir;
    options.skip_refinement = skip_refinement;
    options.patterns_dir = patterns_dir;
    options.github_owner = github_owner;
    options.github_repo = github_repo;
    options.github_token = github_token;
    if (run->count("--design-model") > 0) {
      options.design_model = design_model;
    }
    if (run->count("--github-app-id") > 0) {
      options.github_app_id = github_app_id;
    }
    if (run->count("--github-installation-id") > 0) {
      options.github_installation_id = github_installation_id;
    }
    if (run->count("--github-key-file") > 0) {
      options.github_private_key_file = github_key_file;
    }
    if (run->count("--github-signing-key-file") > 0) {
      options.github_signing_key_file = github_signing_key_file;
    }
    for (const auto& override : model_overrides) {
      const std::size_t equals = override.find('=');
      if (equals == std::string::npos) {
        std::cerr << "--model expects role=id, got '" << override << "'\n";
        return 2;
      }
      options.role_models[override.substr(0, equals)] = override.substr(equals + 1);
    }
    return cf::cli::execute_command(options, std::cout, std::cerr);
  }
  if (*approve) {
    if (!refuse_waiver_id.empty()) {
      if (approve->count("--waiver") > 0 || request_changes) {
        std::cerr << "--refuse-waiver cannot be combined with another "
                     "approval decision\n";
        return 2;
      }
      return cf::cli::refuse_waiver_request_command(
          workspace, run_id, refuse_waiver_id, design_feedback, std::cout,
          std::cerr);
    }
    if (approve->count("--waiver") > 0) {
      if (request_changes) {
        std::cerr << "--waiver and --request-changes are separate decisions; "
                     "use one\n";
        return 2;
      }
      if (approve->count("--stage") > 0) {
        std::cerr << "--waiver and --stage are separate approvals; use one\n";
        return 2;
      }
      cf::cli::WaiverOptions options;
      options.rule = waiver_rule;
      options.scope = waiver_scope;
      options.justification = waiver_justification;
      options.approved_by = waiver_approved_by;
      options.milestone_id = waiver_milestone;
      return cf::cli::approve_waiver_command(workspace, run_id, options,
                                             std::cout, std::cerr);
    }
    if (request_changes) {
      if (approve->count("--scope") > 0 || approve->count("--justification") > 0 ||
          approve->count("--approved-by") > 0 ||
          approve->count("--milestone") > 0) {
        std::cerr << "waiver options cannot be combined with --request-changes\n";
        return 2;
      }
      return cf::cli::request_design_changes_command(
          workspace, run_id, design_feedback, std::cout, std::cerr);
    }
    for (const char* flag :
         {"--scope", "--justification", "--approved-by", "--milestone",
          "--reason"}) {
      if (approve->count(flag) > 0) {
        std::cerr << flag << " is only valid with --waiver or --request-changes\n";
        return 2;
      }
    }
    return cf::cli::approve_command(workspace, run_id, stage, std::cout, std::cerr);
  }
  if (*abort) {
    return cf::cli::abort_command(workspace, run_id, abort_reason, std::cout,
                                  std::cerr);
  }
  if (*ingest) {
    cf::cli::IngestOptions options;
    options.docs_dir = docs_input;
    options.out_dir = ingest_out;
    options.db_path = ingest_db;
    return cf::cli::ingest_command(options, std::cout, std::cerr);
  }
  if (*status) {
    return cf::cli::status_command(workspace, run_id, json, std::cout, std::cerr);
  }
  if (*resume) {
    return cf::cli::resume_command(workspace, run_id, holder, repository_path,
                                   std::cout, std::cerr);
  }
  if (*escalations) {
    return cf::cli::escalations_command(workspace, run_id, std::cout, std::cerr);
  }
  if (*report) {
    return cf::cli::report_command(workspace, run_id, out_dir, std::cout, std::cerr);
  }
  if (*remind) {
    if (run_id.empty() && !remind_all) {
      std::cerr << "remind needs --run or --all\n";
      return 2;
    }
    cf::cli::FrameworkConfig file_config;
    std::string config_file = config_path;
    if (config_file.empty() && std::filesystem::exists("cf.yaml")) {
      config_file = "cf.yaml";
    }
    if (!config_file.empty()) {
      auto loaded = cf::cli::load_config(config_file);
      if (!loaded) {
        std::cerr << loaded.error().code << ": " << loaded.error().message << "\n";
        return 2;
      }
      file_config = std::move(*loaded);
    }
    cf::cli::ExecuteOptions options;
    auto applied = cf::cli::apply_config(options, file_config);
    if (!applied) {
      std::cerr << applied.error().code << ": " << applied.error().message << "\n";
      return 2;
    }
    cf::cli::ReminderOptions reminder_options;
    reminder_options.run_id = run_id;
    reminder_options.all = remind_all;
    reminder_options.workspace =
        remind->count("--workspace") > 0 || options.workspace.empty()
            ? std::filesystem::path(workspace)
            : options.workspace;
    reminder_options.github_owner =
        remind->count("--github-owner") > 0 ? github_owner : options.github_owner;
    reminder_options.github_repo =
        remind->count("--github-repo") > 0 ? github_repo : options.github_repo;
    reminder_options.github_api_base = options.github_api_base;
    reminder_options.github_app_id =
        remind->count("--github-app-id") > 0 ? github_app_id
                                              : options.github_app_id;
    reminder_options.github_installation_id =
        remind->count("--github-installation-id") > 0
            ? github_installation_id
            : options.github_installation_id;
    reminder_options.github_private_key_file =
        remind->count("--github-key-file") > 0
            ? github_key_file
            : options.github_private_key_file;
    reminder_options.github_token =
        remind->count("--github-token") > 0 ? github_token
                                             : options.github_token;
    return cf::cli::reminder_command(reminder_options, std::cout, std::cerr);
  }
  if (*rulesets) {
    cf::cli::RulesetCommandOptions options;
    options.out_dir = rulesets_out;
    options.required_checks = ruleset_checks;
    return cf::cli::rulesets_command(options, std::cout, std::cerr);
  }
  if (*replay) {
    cf::cli::ReplayOptions replay_options;
    replay_options.workspace = workspace;
    replay_options.run_id = run_id;
    replay_options.call_id = replay_call;
    replay_options.model = replay_model;
    replay_options.base_url = replay_base_url;
    replay_options.api_key_env = replay_api_key_env;
    return cf::cli::replay_command(replay_options, std::cout, std::cerr);
  }
  if (*cleanup) {
    if (run_id.empty() && !cleanup_all) {
      std::cerr << "cleanup needs --run or --all\n";
      return 2;
    }
    cf::cli::FrameworkConfig file_config;
    std::string config_file = config_path;
    if (config_file.empty() && std::filesystem::exists("cf.yaml")) {
      config_file = "cf.yaml";
    }
    if (!config_file.empty()) {
      auto loaded = cf::cli::load_config(config_file);
      if (!loaded) {
        std::cerr << loaded.error().code << ": " << loaded.error().message << "\n";
        return 2;
      }
      file_config = std::move(*loaded);
    }
    cf::cli::ExecuteOptions options;
    auto applied = cf::cli::apply_config(options, file_config);
    if (!applied) {
      std::cerr << applied.error().code << ": " << applied.error().message << "\n";
      return 2;
    }

    cf::cli::CleanupOptions cleanup_options;
    cleanup_options.run_id = run_id;
    cleanup_options.all = cleanup_all;
    cleanup_options.workspace =
        cleanup->count("--workspace") > 0 || options.workspace.empty()
            ? std::filesystem::path(workspace)
            : options.workspace;
    cleanup_options.repository_path =
        cleanup->count("--repo") > 0 ? std::filesystem::path(repository_path)
                                     : options.repository_path;
    cleanup_options.github_owner =
        cleanup->count("--github-owner") > 0 ? github_owner
                                              : options.github_owner;
    cleanup_options.github_repo =
        cleanup->count("--github-repo") > 0 ? github_repo : options.github_repo;
    cleanup_options.github_api_base = options.github_api_base;
    cleanup_options.github_app_id =
        cleanup->count("--github-app-id") > 0 ? github_app_id
                                               : options.github_app_id;
    cleanup_options.github_installation_id =
        cleanup->count("--github-installation-id") > 0
            ? github_installation_id
            : options.github_installation_id;
    cleanup_options.github_private_key_file =
        cleanup->count("--github-key-file") > 0
            ? github_key_file
            : options.github_private_key_file;
    cleanup_options.github_token =
        cleanup->count("--github-token") > 0 ? github_token
                                              : options.github_token;
    cleanup_options.github_remote = github_remote;
    cleanup_options.retention_days =
        cleanup->count("--retention-days") > 0
            ? retention_days
            : options.branch_retention_days.value_or(30);
    return cf::cli::cleanup_command(cleanup_options, std::cout, std::cerr);
  }
  std::cout << app.help() << "\n";
  return 0;
}
