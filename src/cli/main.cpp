// cf: the Coder Framework command line (design section 9.2).
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "cf/cli/commands.hpp"

int main(int argc, char** argv) {
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
  std::string docs_dir;
  bool skip_refinement = false;
  std::vector<std::string> model_overrides;
  std::string github_owner;
  std::string github_repo;
  std::string github_app_id;
  std::string github_installation_id;
  std::string github_key_file;
  std::string github_token;
  run->add_flag("--execute", execute, "Execute the run after creating it");
  run->add_option("--repo", repository_path, "Target repository (with --execute)");
  run->add_option("--opencode", opencode_binary, "opencode binary");
  run->add_option("--mcp", mcp_binary, "cf-mcp binary");
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
  run->add_option("--github-token", github_token, "Static GitHub token");

  auto* approve = app.add_subcommand("approve", "Approve a run stage (design gate)");
  std::string stage = "design";
  approve->add_option("--run", run_id, "Run id")->required();
  approve->add_option("--workspace", workspace, "Workspace directory");
  approve->add_option("--stage", stage, "Approval stage (design)");

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

  CLI11_PARSE(app, argc, argv);

  if (*check) {
    return cf::cli::check_command(plan_path, profiles_dir, std::cout, std::cerr);
  }
  if (*run) {
    const int created = cf::cli::run_command(plan_path, profiles_dir, workspace, run_id,
                                             std::cout, std::cerr);
    if (created != 0 || !execute) {
      return created;
    }
    cf::cli::ExecuteOptions options;
    options.workspace = workspace;
    options.run_id = run_id;
    options.profiles_dir = profiles_dir;
    options.repository_path = repository_path;
    options.opencode_binary = opencode_binary;
    options.mcp_binary = mcp_binary;
    options.sandbox_image = sandbox_image;
    options.docs_dir = docs_dir;
    options.skip_refinement = skip_refinement;
    options.design_model = design_model;
    options.patterns_dir = patterns_dir;
    options.github_owner = github_owner;
    options.github_repo = github_repo;
    options.github_app_id = github_app_id;
    options.github_installation_id = github_installation_id;
    options.github_private_key_file = github_key_file;
    options.github_token = github_token;
    // Pinned defaults from third_party/VERSIONS; override with --model.
    options.role_models = {
        {"coder", "accounts/fireworks/models/deepseek-v4p1-flash"},
        {"reviewer", "accounts/fireworks/models/kimi-k3"},
        {"qa", "accounts/fireworks/models/glm-5p3"}};
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
    return cf::cli::approve_command(workspace, run_id, stage, std::cout, std::cerr);
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
  std::cout << app.help() << "\n";
  return 0;
}
