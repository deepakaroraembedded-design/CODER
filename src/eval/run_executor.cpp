#include "cf/eval/run_executor.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cf/eval/hidden.hpp"
#include "cf/gates/profile.hpp"
#include "cf/git/repository.hpp"
#include "cf/orch/run_process.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/store/run_store.hpp"

namespace cf::eval {
namespace {

using Json = nlohmann::json;

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

// Copies `seed` into `repo` (preserving permissions) and returns the relative
// paths of everything copied, for the initial commit.
std::vector<std::string> copy_seed_tree(const std::filesystem::path& seed,
                                        const std::filesystem::path& repo) {
  std::vector<std::string> paths;
  std::error_code ec;
  if (!std::filesystem::is_directory(seed, ec)) {
    return paths;
  }
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(seed, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    const std::filesystem::path relative =
        std::filesystem::relative(entry.path(), seed, ec);
    if (ec) {
      continue;
    }
    const std::filesystem::path target = repo / relative;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::copy_file(entry.path(), target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) {
      paths.push_back(relative.generic_string());
    }
  }
  return paths;
}

// A plan id becomes a git-branch-safe run id.
std::string sanitize(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '-' ||
                      character == '_';
    out.push_back(safe ? character : '-');
  }
  return out;
}

// The agents retrieve requirements through the MCP tools; for a golden plan
// the plan itself is the requirements source (design section 6.3), so it is
// written as a front-matter document the retrieval index can serve.
std::string requirements_document(const plan::Plan& plan) {
  std::string out = "---\ndoc_id: " + plan.plan_id +
                    "-requirements\nversion: \"1.0\"\ntype: requirements\n"
                    "owner: golden-suite\nstatus: active\n---\n\n# Requirements for " +
                    plan.plan_id + "\n\n";
  for (const auto& milestone : plan.milestones) {
    if (milestone.requirements.empty()) {
      continue;
    }
    out += "## Milestone " + milestone.id + ": " + milestone.title + "\n\n";
    for (const auto& requirement : milestone.requirements) {
      out += "### " + requirement + "\n\n";
      for (const auto& criterion : milestone.acceptance) {
        out += "- " + criterion + "\n";
      }
      out += "\n";
    }
  }
  return out;
}

std::int64_t number_or_zero(const Json& node, const char* key) {
  const Json value = node.value(key, Json());
  if (value.is_number_integer()) {
    return value.get<std::int64_t>();
  }
  if (value.is_number_float()) {
    return static_cast<std::int64_t>(value.get<double>());
  }
  return 0;
}

struct UsageTotals {
  double cost_usd = 0.0;
  std::int64_t prompt_tokens = 0;
  std::int64_t completion_tokens = 0;
};

// Reads the `usage` payload of one session (opencode's session info: `cost`
// and a `tokens` object; field names vary across providers, so several shapes
// are accepted).
UsageTotals usage_totals(const Json& usage) {
  UsageTotals totals;
  const Json cost = usage.value("cost", Json());
  if (cost.is_number()) {
    totals.cost_usd += cost.get<double>();
  }
  const Json tokens = usage.value("tokens", Json());
  if (tokens.is_object()) {
    totals.prompt_tokens += number_or_zero(tokens, "input") +
                            number_or_zero(tokens, "prompt");
    totals.completion_tokens += number_or_zero(tokens, "output") +
                                number_or_zero(tokens, "completion");
  } else if (tokens.is_number_integer()) {
    totals.prompt_tokens += tokens.get<std::int64_t>();
  }
  return totals;
}

}  // namespace

std::string RunProcessExecutor::run_id_for(const std::string& plan_id, int repeat) {
  return "bench-" + sanitize(plan_id) + "-r" + std::to_string(repeat);
}

std::filesystem::path RunProcessExecutor::scratch_directory(const std::string& plan_id,
                                                            int repeat) const {
  return options_.workspace_root / sanitize(plan_id) / ("repeat-" + std::to_string(repeat));
}

RunProcessExecutor::RunProcessExecutor(RunExecutorOptions options,
                                       process::ProcessExecutor& executor)
    : options_(std::move(options)), executor_(executor) {
  // Every derived path (worktrees, builds, reports) is handed to git and the
  // run process, which may run with a different working directory; anchor the
  // configuration to absolute paths once, here.
  const auto absolutize = [](std::filesystem::path& path) {
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (!ec) {
      path = absolute;
    }
  };
  absolutize(options_.workspace_root);
  absolutize(options_.suite_dir);
  absolutize(options_.profiles_dir);
  absolutize(options_.hidden_dir);
  if (!options_.docs_dir.empty()) {
    std::filesystem::path docs = options_.docs_dir;
    absolutize(docs);
    options_.docs_dir = docs.string();
  }
}

Result<PlanOutcome> RunProcessExecutor::run(const PlanEntry& plan_entry, int repeat) {
  if (options_.workspace_root.empty()) {
    return fail(error_codes::kRunExecutorConfig, "no workspace root for the run executor");
  }
  if (options_.mcp_binary.empty()) {
    return fail(error_codes::kRunExecutorConfig, "no cf-mcp binary for the run executor");
  }

  PlanOutcome outcome;
  outcome.plan_id = plan_entry.id;
  outcome.repeat = repeat;

  const std::filesystem::path plan_path = options_.suite_dir / plan_entry.plan_path;
  auto plan = plan::parse_plan_file(plan_path);
  if (!plan.has_value()) {
    return fail(error_codes::kRunExecutorFailed,
                "plan '" + plan_entry.id + "': " + plan.error().message);
  }

  auto profiles = gates::ProfileRegistry::load_dir(options_.profiles_dir);
  if (!profiles.has_value()) {
    return fail(error_codes::kRunExecutorFailed, profiles.error().message);
  }
  auto profile = profiles->resolve(plan->target.language, plan->target.build_system,
                                   plan->target.test_framework);
  if (!profile.has_value()) {
    return fail(error_codes::kRunExecutorFailed,
                "no toolchain profile for plan '" + plan_entry.id + "' (" +
                    profile.error().message + ")");
  }

  const std::filesystem::path scratch = scratch_directory(plan_entry.id, repeat);
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);
  if (ec) {
    return fail(error_codes::kRunExecutorFailed,
                "cannot create scratch directory " + scratch.string() + ": " +
                    ec.message());
  }

  const std::string run_id = run_id_for(plan_entry.id, repeat);
  const std::filesystem::path hidden_base =
      options_.hidden_dir.empty() ? options_.suite_dir / "hidden" : options_.hidden_dir;

  // The MCP retrieval index: the plan's requirements plus any caller-provided
  // documents, in the scratch directory so every repeat is self-contained.
  const std::filesystem::path docs_dir = scratch / "docs";
  std::filesystem::create_directories(docs_dir, ec);
  write_file(docs_dir / "requirements.md", requirements_document(*plan));
  if (!options_.docs_dir.empty()) {
    (void)copy_seed_tree(options_.docs_dir, docs_dir);
  }

  // A fresh target repository on the base branch; scenarios may seed it with
  // fixtures and injection payloads (design section 17.2).
  auto repository = git::Repository::create(scratch / "repo", executor_);
  if (!repository.has_value()) {
    return fail(error_codes::kRunExecutorFailed, repository.error().message);
  }
  write_file(scratch / "repo" / "README.md", "# " + plan->product + "\n");
  std::vector<std::string> initial_paths = {"README.md"};
  const std::vector<std::string> seeded =
      copy_seed_tree(hidden_base / plan_entry.id / "seed", scratch / "repo");
  initial_paths.insert(initial_paths.end(), seeded.begin(), seeded.end());
  git::CommitTrailers init;
  init.run_id = run_id;
  init.op_id = "op_bench_init";
  auto initial = repository->commit_paths(scratch / "repo", initial_paths,
                                          "chore: initial commit", init);
  if (!initial.has_value()) {
    return fail(error_codes::kRunExecutorFailed, initial.error().message);
  }

  // The run store and its milestones.
  const std::filesystem::path run_dir = scratch / "runs" / run_id;
  std::filesystem::create_directories(run_dir, ec);
  auto store = store::RunStore::open(run_dir / "run.db");
  if (!store.has_value()) {
    return fail(error_codes::kRunExecutorFailed, store.error().message);
  }
  store::RunRecord run;
  run.run_id = run_id;
  run.plan_id = plan->plan_id;
  run.state = "Created";
  run.created_at = store::unix_now();
  run.config_json = Json{{"plan_path", std::filesystem::absolute(plan_path).string()},
                         {"benchmark_plan_id", plan_entry.id}}
                        .dump();
  auto created = (*store)->create_run(run);
  if (!created.has_value()) {
    return fail(error_codes::kRunExecutorFailed, created.error().message);
  }
  write_file(run_dir / "plan.yaml", [&plan_path]() {
    std::ifstream in(plan_path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }());

  orch::StateMachine machine(**store, run_id);
  for (const auto& milestone : plan->milestones) {
    auto registered = machine.register_milestone(milestone.id);
    if (!registered.has_value()) {
      return fail(error_codes::kRunExecutorFailed, registered.error().message);
    }
  }
  // Design refinement is not implemented yet (Phase 4); advance to Executing
  // exactly like `cf run --execute --skip-refinement`.
  for (const orch::RunEvent event : {orch::RunEvent::Start, orch::RunEvent::IndexReady,
                                     orch::RunEvent::DesignPackageReady,
                                     orch::RunEvent::Approved}) {
    auto applied = machine.apply(event, {}, store::unix_now());
    if (!applied.has_value()) {
      return fail(error_codes::kRunExecutorFailed, applied.error().message);
    }
  }

  // The production run process with the candidate's models.
  orch::RunProcessConfig config;
  config.run_id = run_id;
  config.workspace_root = scratch;
  config.integration_branch = "cf/" + run_id + "/integration";
  config.base_branch = options_.base_branch;
  config.opencode_binary = options_.opencode_binary;
  config.provider_base_url = options_.provider_base_url;
  config.api_key_env = options_.api_key_env;
  config.role_models = options_.role_models;
  config.role_options = options_.role_options;
  config.mcp_command = options_.mcp_binary;
  config.docs_dir = docs_dir.string();
  config.environment = options_.environment;
  config.turn_timeout = options_.turn_timeout;
  config.profile = **profile;
  config.profile_language = plan->target.language;
  config.profile_build_system = plan->target.build_system;
  config.profile_test_framework = plan->target.test_framework;
  config.sandbox_image = options_.sandbox_image;
  config.step_timeout = options_.step_timeout;
  config.orchestrator.escalation_dir = (run_dir / "escalations").string();
  if (plan->budget.max_usd_total.has_value()) {
    config.orchestrator.max_usd_total = plan->budget.max_usd_total;
  }
  if (plan->budget.max_tokens_total.has_value()) {
    config.orchestrator.max_tokens_total = plan->budget.max_tokens_total;
  }

  spdlog::info("benchmark: running plan {} repeat {} as {}", plan_entry.id, repeat,
               run_id);
  const auto started = std::chrono::steady_clock::now();
  orch::RunProcess process(**store, *plan, config, *repository, executor_);
  auto final_state = process.execute(store::unix_now());
  outcome.duration_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (!final_state.has_value()) {
    return fail(error_codes::kRunExecutorFailed,
                "run " + run_id + ": " + final_state.error().message);
  }

  // Metrics, straight from the store.
  const std::string final = std::string(orch::to_string(*final_state));
  outcome.terminal = final == "Done" || final == "Escalated" || final == "Aborted";
  auto milestones = (*store)->list_milestones(run_id);
  if (!milestones.has_value()) {
    return fail(error_codes::kRunExecutorFailed, milestones.error().message);
  }
  for (const auto& milestone : *milestones) {
    ++outcome.milestones_total;
    if (milestone.state == "Done") {
      ++outcome.milestones_done;
    }
    outcome.iterations += static_cast<int>(milestone.iterations);
  }
  auto usd = (*store)->total_usd(run_id);
  outcome.cost_usd = usd.has_value() ? *usd : 0.0;
  outcome.cost_usd +=
      sum_agent_usage_usd(run_dir, options_.role_models, options_.prices);

  // Escalation bundles, with their reasons.
  const std::filesystem::path escalations = run_dir / "escalations";
  if (std::filesystem::is_directory(escalations, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(escalations, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("esc-", 0) != 0) {
        continue;
      }
      ++outcome.escalations;
      std::ifstream bundle(entry.path() / "bundle.json", std::ios::binary);
      if (bundle) {
        Json parsed =
            Json::parse(std::string(std::istreambuf_iterator<char>(bundle),
                                    std::istreambuf_iterator<char>()),
                        nullptr, false);
        if (!parsed.is_discarded() && parsed.contains("reason")) {
          outcome.escalation_reasons.push_back(parsed["reason"].get<std::string>());
        }
      }
    }
  }

  // Hidden acceptance tests against the integration worktree.
  const std::filesystem::path hidden_spec_path =
      hidden_base / (plan_entry.id + "/spec.yaml");
  std::error_code hidden_ec;
  if (std::filesystem::exists(hidden_spec_path, hidden_ec)) {
    auto spec = load_hidden_test_spec(hidden_spec_path);
    if (!spec.has_value()) {
      return fail(error_codes::kRunExecutorFailed, spec.error().message);
    }
    const std::filesystem::path worktree =
        scratch / "wt" / run_id / "integration";
    if (!std::filesystem::is_directory(worktree, hidden_ec)) {
      spdlog::warn("benchmark: no integration worktree for {}, hidden tests failed",
                   run_id);
      outcome.hidden_tests_total = static_cast<int>(spec->tests.size());
      outcome.hidden_tests_passed = 0;
    } else {
      const auto hidden = run_hidden_tests(*spec, worktree, scratch / "hidden-build",
                                           executor_, options_.hidden_timeout);
      outcome.hidden_tests_total = hidden.tests;
      outcome.hidden_tests_passed = hidden.passed;
      for (const auto& line : hidden.log) {
        spdlog::info("hidden {}: {}", plan_entry.id, line);
      }
    }
    if (spec->security) {
      outcome.security_passed = outcome.hidden_tests_total > 0 &&
                                outcome.hidden_tests_passed == outcome.hidden_tests_total;
    }
    if (spec->recovery) {
      outcome.recovery_ok = outcome.hidden_tests_total > 0 &&
                            outcome.hidden_tests_passed == outcome.hidden_tests_total;
    }
  }

  if (!options_.keep_scratch) {
    std::filesystem::remove_all(scratch, ec);
  }
  return outcome;
}

double sum_agent_usage_usd(
    const std::filesystem::path& run_dir,
    const std::map<std::string, std::string, std::less<>>& role_models,
    const std::map<std::string, ModelPrice, std::less<>>& prices) {
  double total = 0.0;
  std::error_code ec;
  if (!std::filesystem::is_directory(run_dir, ec)) {
    return total;
  }
  for (const auto& entry : std::filesystem::directory_iterator(run_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("usage-", 0) != 0 || entry.path().extension() != ".jsonl") {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      Json parsed = Json::parse(line, nullptr, false);
      if (parsed.is_discarded() || !parsed.is_object()) {
        continue;
      }
      const Json usage = parsed.value("usage", Json::object());
      const UsageTotals totals = usage_totals(usage);
      if (totals.cost_usd > 0.0) {
        total += totals.cost_usd;
        continue;
      }
      // No provider cost: estimate from the candidate's price table.
      const std::string role = parsed.value("role", "");
      const auto model = role_models.find(role);
      if (model == role_models.end()) {
        continue;
      }
      const auto price = prices.find(model->second);
      if (price == prices.end()) {
        continue;
      }
      total += static_cast<double>(totals.prompt_tokens) / 1e6 *
                   price->second.usd_per_mtok_in +
               static_cast<double>(totals.completion_tokens) / 1e6 *
                   price->second.usd_per_mtok_out;
    }
  }
  return total;
}

}  // namespace cf::eval
