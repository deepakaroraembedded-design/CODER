#include "cf/cli/commands.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "cf/common/hash.hpp"
#include "cf/common/process.hpp"
#include "cf/gates/profile.hpp"
#include "cf/git/repository.hpp"
#include "cf/github/app_auth.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/orch/recovery.hpp"
#include "cf/orch/run_process.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/rag/catalog.hpp"
#include "cf/rag/injection_scan.hpp"
#include "cf/rag/loader.hpp"
#include "cf/rag/retriever.hpp"
#include "cf/store/run_store.hpp"

namespace cf::cli {
namespace {

using Json = nlohmann::json;

const std::vector<std::string> kOpenDefectStatuses = {
    "OPEN", "IN_PROGRESS", "FIXED_PENDING_VERIFY", "REOPENED", "DISPUTED"};

std::filesystem::path store_path(const std::filesystem::path& workspace,
                                 const std::string& run_id) {
  return run_directory(workspace, run_id) / "run.db";
}

Json milestone_json(const store::MilestoneRecord& milestone) {
  return Json{{"id", milestone.milestone_id},
              {"state", milestone.state},
              {"iterations", milestone.iterations}};
}

Json gate_rows_json(const std::vector<store::GateResultRow>& rows) {
  Json gates = Json::array();
  for (const auto& row : rows) {
    gates.push_back(Json{{"gate", row.gate},
                         {"passed", row.passed},
                         {"commit_sha", row.commit_sha},
                         {"at", row.at}});
  }
  return gates;
}

Json defect_json(const store::DefectRecord& defect) {
  return Json{{"id", defect.id},
              {"milestone_id", defect.milestone_id},
              {"status", defect.status},
              {"severity", defect.severity},
              {"attempts", defect.attempts},
              {"body", Json::parse(defect.body_json, nullptr, false)}};
}

// Loads the profile registry; an empty or missing directory is not an error
// (FR-18 is only enforced when profiles are provided).
Result<gates::ProfileRegistry> load_profiles(const std::filesystem::path& dir) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) {
    return gates::ProfileRegistry{};
  }
  return gates::ProfileRegistry::load_dir(dir);
}

plan::ValidationOptions profile_check(const gates::ProfileRegistry& registry) {
  plan::ValidationOptions options;
  const gates::ProfileRegistry* pointer = &registry;
  options.profile_exists = [pointer](const plan::TargetSpec& target) {
    return pointer
        ->resolve(target.language, target.build_system, target.test_framework)
        .has_value();
  };
  return options;
}

std::string resolve_profile_id(const gates::ProfileRegistry& registry,
                               const plan::Plan& plan) {
  auto profile = registry.resolve(plan.target.language, plan.target.build_system,
                                  plan.target.test_framework);
  return profile.has_value() ? (*profile)->id : std::string();
}

std::string read_file_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// Bridges the planner model over cf-llm (design section 6.2): the refiner
// makes direct model calls, not opencode agent turns.
class LlmDesignModel final : public orch::DesignModel {
 public:
  LlmDesignModel(llm::FireworksClient& client, std::string model)
      : client_(client), model_(std::move(model)) {}

  Result<std::string> complete(std::string_view prompt) override {
    llm::ChatRequest request;
    request.model = model_;
    llm::ChatMessage message;
    message.role = "user";
    message.content = std::string(prompt);
    request.messages.push_back(std::move(message));
    request.temperature = 0.2;
    request.max_tokens = 8192;
    auto response = client_.chat(request);
    if (!response) {
      return tl::make_unexpected(response.error());
    }
    return response->content;
  }

 private:
  llm::FireworksClient& client_;
  std::string model_;
};

// Runs the refinement pipeline for a run in Refining: an existing design
// package is reused (so a retry after a crash never plans twice), otherwise
// the planner model produces one, validated and stored as design.json plus a
// human-readable design.md (design section 6.2).
Result<orch::DesignPackage> refine_or_load(const ExecuteOptions& options,
                                           const std::filesystem::path& run_dir,
                                           const plan::Plan& plan,
                                           std::ostream& out) {
  const std::filesystem::path design_path = run_dir / "design.json";
  std::error_code ec;
  if (std::filesystem::exists(design_path, ec)) {
    auto existing = orch::parse_design_package(read_file_text(design_path));
    if (existing) {
      return existing;
    }
    spdlog::warn("stored design package is unusable ({}); refining again",
                 existing.error().message);
  }

  std::string model_id = options.design_model;
  if (model_id.empty()) {
    const auto coder = options.role_models.find("coder");
    if (coder != options.role_models.end()) {
      model_id = coder->second;
    }
  }
  std::unique_ptr<llm::FireworksClient> client;
  std::unique_ptr<LlmDesignModel> model;
  orch::DesignModel* active = options.design_model_override;
  if (active == nullptr) {
    if (model_id.empty()) {
      return fail("ORCH_REFINE_FAILED",
                  "no planner model: pass --design-model or a coder --model");
    }
    const char* key = std::getenv(options.api_key_env.c_str());
    if (key == nullptr || *key == '\0') {
      return fail("ORCH_REFINE_FAILED",
                  options.api_key_env + " is not set (needed for design refinement)");
    }
    llm::ClientOptions client_options;
    client_options.base_url = options.provider_base_url;
    client_options.api_key = key;
    client = std::make_unique<llm::FireworksClient>(
        client_options, std::make_unique<llm::CprTransport>());
    model = std::make_unique<LlmDesignModel>(*client, model_id);
    active = model.get();
  }

  // Company documents plus approved design packages from earlier runs
  // (design section 6.2 step 7); the applicability filter keeps only the
  // ones scoped to this plan's product.
  const std::filesystem::path designs_dir = options.workspace / "designs";
  std::unique_ptr<rag::InMemoryRetriever> retriever;
  if (!options.docs_dir.empty() || std::filesystem::is_directory(designs_dir)) {
    retriever = std::make_unique<rag::InMemoryRetriever>();
    for (const auto& directory :
         {std::filesystem::path(options.docs_dir), designs_dir}) {
      if (directory.empty()) {
        continue;
      }
      auto documents = rag::load_document_dir(directory);
      if (!documents) {
        return fail("ORCH_REFINE_FAILED",
                    "cannot load documents for refinement from " +
                        directory.string() + ": " + documents.error().message);
      }
      for (auto& document : *documents) {
        auto added = retriever->add_document(std::move(document));
        if (!added) {
          return fail("ORCH_REFINE_FAILED", added.error().message);
        }
      }
    }
  }

  // The curated pattern catalog (design section 6.2), when present.
  std::vector<rag::PatternEntry> catalog;
  if (!options.patterns_dir.empty() &&
      std::filesystem::is_directory(options.patterns_dir)) {
    auto loaded = rag::load_pattern_catalog(options.patterns_dir);
    if (!loaded) {
      return fail("ORCH_REFINE_FAILED", loaded.error().message);
    }
    catalog = std::move(*loaded);
  }

  orch::DesignRefinerOptions refiner_options;
  refiner_options.model = model_id;
  orch::DesignRefiner refiner(*active, retriever.get(), std::move(refiner_options),
                              &catalog);
  auto package = refiner.refine(plan);
  if (!package) {
    return package;
  }
  {
    std::ofstream json_out(design_path, std::ios::binary);
    json_out << orch::design_package_to_json(*package);
  }
  {
    std::ofstream markdown_out(run_dir / "design.md", std::ios::binary);
    markdown_out << orch::design_package_to_markdown(*package);
  }
  out << "design package written: " << design_path.string() << "\n";
  return package;
}

}  // namespace

std::filesystem::path run_directory(const std::filesystem::path& workspace,
                                    const std::string& run_id) {
  return workspace / "runs" / run_id;
}

int check_command(const std::filesystem::path& plan_path,
                  const std::filesystem::path& profiles_dir, std::ostream& out,
                  std::ostream& err) {
  auto profiles = load_profiles(profiles_dir);
  if (!profiles) {
    err << profiles.error().code << ": " << profiles.error().message << "\n";
    return 2;
  }
  auto plan = plan::parse_plan_file(plan_path, profile_check(*profiles));
  if (!plan) {
    err << plan.error().code << ": " << plan.error().message << "\n";
    return 1;
  }
  Json milestones = Json::array();
  for (const auto& milestone : plan->milestones) {
    milestones.push_back(milestone.id);
  }
  out << Json{{"plan_id", plan->plan_id},
              {"product", plan->product},
              {"milestones", std::move(milestones)},
              {"target", Json{{"language", plan->target.language},
                              {"standard", plan->target.standard},
                              {"build_system", plan->target.build_system},
                              {"test_framework", plan->target.test_framework}}},
              {"profile", resolve_profile_id(*profiles, *plan)}}
             .dump(2)
      << "\n";
  return 0;
}

int run_command(const std::filesystem::path& plan_path,
                const std::filesystem::path& profiles_dir,
                const std::filesystem::path& workspace, const std::string& run_id_in,
                std::ostream& out, std::ostream& err) {
  auto profiles = load_profiles(profiles_dir);
  if (!profiles) {
    err << profiles.error().code << ": " << profiles.error().message << "\n";
    return 2;
  }
  auto plan = plan::parse_plan_file(plan_path, profile_check(*profiles));
  if (!plan) {
    err << plan.error().code << ": " << plan.error().message << "\n";
    return 1;
  }

  const std::string run_id =
      run_id_in.empty() ? "R" + std::to_string(store::unix_now()) : run_id_in;
  const std::filesystem::path directory = run_directory(workspace, run_id);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    err << "cannot create run directory: " << ec.message() << "\n";
    return 2;
  }

  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }

  // Re-running an existing run is not an error: the stored plan drives it
  // (for example `cf run --execute` again after `cf approve`). Nothing is
  // recreated or overwritten.
  auto existing = (*store)->get_run(run_id);
  if (existing.has_value()) {
    if (existing->plan_id != plan->plan_id) {
      err << "note: run " << run_id << " is bound to plan " << existing->plan_id
          << "; ignoring " << plan->plan_id << "\n";
    }
    auto milestones = (*store)->list_milestones(run_id);
    Json ids = Json::array();
    if (milestones.has_value()) {
      for (const auto& milestone : *milestones) {
        ids.push_back(milestone.milestone_id);
      }
    }
    out << Json{{"run_id", run_id},
                {"state", existing->state},
                {"plan_id", existing->plan_id},
                {"milestones", std::move(ids)},
                {"store", store_path(workspace, run_id).string()}}
                   .dump(2)
        << "\n";
    return 0;
  }
  if (existing.error().code != store::error_codes::kNotFound) {
    err << existing.error().code << ": " << existing.error().message << "\n";
    return 1;
  }

  std::filesystem::copy_file(plan_path, directory / "plan.yaml",
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    err << "cannot copy the plan into the run directory: " << ec.message() << "\n";
    return 2;
  }

  store::RunRecord run;
  run.run_id = run_id;
  run.plan_id = plan->plan_id;
  run.state = "Created";
  run.created_at = store::unix_now();
  run.config_json = Json{{"plan_path", std::filesystem::absolute(plan_path).string()},
                         {"profile", resolve_profile_id(*profiles, *plan)}}
                        .dump();
  const auto created = (*store)->create_run(run);
  if (!created) {
    err << created.error().code << ": " << created.error().message << "\n";
    return 1;
  }

  orch::StateMachine machine(**store, run_id);
  for (const auto& spec : plan->milestones) {
    const auto registered = machine.register_milestone(spec.id);
    if (!registered) {
      err << registered.error().code << ": " << registered.error().message << "\n";
      return 2;
    }
  }

  Json milestones = Json::array();
  for (const auto& spec : plan->milestones) {
    milestones.push_back(spec.id);
  }
  out << Json{{"run_id", run_id},
              {"state", "Created"},
              {"plan_id", plan->plan_id},
              {"milestones", std::move(milestones)},
              {"store", store_path(workspace, run_id).string()}}
             .dump(2)
      << "\n";
  return 0;
}

int status_command(const std::filesystem::path& workspace, const std::string& run_id,
                   bool json_out, std::ostream& out, std::ostream& err) {
  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto run = (*store)->get_run(run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  auto milestones = (*store)->list_milestones(run_id);
  auto operations = (*store)->list_operations(run_id, {});
  auto transitions = (*store)->list_transitions(run_id, 10);
  auto usd = (*store)->total_usd(run_id);
  auto tokens = (*store)->total_tokens(run_id);
  auto locks = (*store)->list_locks(run_id);
  if (!milestones || !operations || !transitions || !usd || !tokens || !locks) {
    err << "cannot read the run store\n";
    return 2;
  }

  Json milestone_rows = Json::array();
  for (const auto& milestone : *milestones) {
    Json row = milestone_json(milestone);
    auto gates = (*store)->latest_gate_results(run_id, milestone.milestone_id);
    row["gates"] = gates.has_value() ? gate_rows_json(*gates) : Json::array();
    auto defects = (*store)->list_defects(run_id, milestone.milestone_id,
                                          kOpenDefectStatuses);
    row["open_defects"] = defects.has_value() ? defects->size() : 0;
    milestone_rows.push_back(std::move(row));
  }

  std::map<std::string, std::size_t, std::less<>> operation_counts;
  for (const auto& operation : *operations) {
    ++operation_counts[operation.state];
  }
  Json operation_summary = Json::object();
  for (const auto& [state, count] : operation_counts) {
    operation_summary[state] = count;
  }

  Json transition_rows = Json::array();
  for (const auto& transition : *transitions) {
    transition_rows.push_back(Json{{"from", transition.from_state},
                                   {"to", transition.to_state},
                                   {"event", transition.event_json},
                                   {"at", transition.at}});
  }

  Json lease = Json();
  auto current_lease = (*store)->get_lease(run_id);
  if (current_lease.has_value()) {
    lease = Json{{"holder", current_lease->holder},
                 {"epoch", current_lease->epoch},
                 {"renewed_at", current_lease->renewed_at}};
  }

  Json document = Json{{"run_id", run->run_id},
                       {"state", run->state},
                       {"plan_id", run->plan_id},
                       {"created_at", run->created_at},
                       {"milestones", std::move(milestone_rows)},
                       {"operations", std::move(operation_summary)},
                       {"locks", locks->size()},
                       {"lease", std::move(lease)},
                       {"cost", Json{{"usd", *usd}, {"tokens", *tokens}}},
                       {"recent_transitions", std::move(transition_rows)}};

  if (json_out) {
    out << document.dump(2) << "\n";
    return 0;
  }

  out << "Run " << run->run_id << " [" << run->state << "] plan=" << run->plan_id
      << " cost=$" << *usd << " tokens=" << *tokens << "\n";
  for (const auto& row : document["milestones"]) {
    out << "  milestone " << row["id"].get<std::string>() << " "
        << row["state"].get<std::string>()
        << " iterations=" << row["iterations"].get<unsigned>();
    if (row["open_defects"].get<std::size_t>() > 0) {
      out << " open_defects=" << row["open_defects"].get<std::size_t>();
    }
    if (!row["gates"].empty()) {
      bool passed = true;
      for (const auto& gate : row["gates"]) {
        passed = passed && gate["passed"].get<bool>();
      }
      out << " gates=" << (passed ? "passed" : "failed");
    }
    out << "\n";
  }
  if (!operation_counts.empty()) {
    out << "  operations:";
    for (const auto& [state, count] : operation_counts) {
      out << " " << state << "=" << count;
    }
    out << "\n";
  }
  return 0;
}

int resume_command(const std::filesystem::path& workspace, const std::string& run_id,
                   const std::string& holder, const std::filesystem::path& repository_path,
                   std::ostream& out, std::ostream& err) {
  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto run = (*store)->get_run(run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }

  std::unique_ptr<orch::RecoveryHook> hook;
  std::unique_ptr<git::Repository> repository;
  if (!repository_path.empty()) {
    process::LocalProcessExecutor executor;
    auto opened = git::Repository::open(repository_path, executor);
    if (!opened) {
      err << opened.error().code << ": " << opened.error().message << "\n";
      return 2;
    }
    repository = std::make_unique<git::Repository>(std::move(*opened));
    hook = std::make_unique<orch::GitWorktreeHook>(*repository, workspace);
  }

  orch::Recovery recovery(**store, std::move(hook));
  orch::Dispatcher dispatcher(**store, run_id, holder);
  orch::RecoveryOptions options;
  options.holder = holder;
  auto report = recovery.resume(run_id, dispatcher, options, store::unix_now());
  if (!report) {
    err << report.error().code << ": " << report.error().message << "\n";
    return 1;
  }
  out << report->to_json() << "\n";
  return 0;
}

int escalations_command(const std::filesystem::path& workspace,
                        const std::string& run_id, std::ostream& out,
                        std::ostream& err) {
  const std::filesystem::path directory = run_directory(workspace, run_id) / "escalations";
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    out << "[]\n";
    return 0;
  }
  std::vector<std::filesystem::path> bundles;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    const auto path = entry.path() / "bundle.json";
    if (std::filesystem::is_regular_file(path, ec)) {
      bundles.push_back(path);
    }
  }
  if (ec) {
    err << "cannot list escalations: " << ec.message() << "\n";
    return 2;
  }
  std::sort(bundles.begin(), bundles.end());
  Json array = Json::array();
  for (const auto& path : bundles) {
    std::ifstream in(path);
    const Json bundle = Json::parse(in, nullptr, false);
    if (bundle.is_discarded()) {
      array.push_back(Json{{"bundle", path.string()}, {"error", "invalid JSON"}});
      continue;
    }
    array.push_back(Json{{"bundle", path.parent_path().filename().string()},
                         {"reason", bundle.value("reason", "")},
                         {"at", bundle.value("at", 0)},
                         {"run_state", bundle.value("run_state", "")}});
  }
  out << array.dump(2) << "\n";
  return 0;
}

int report_command(const std::filesystem::path& workspace, const std::string& run_id,
                   const std::filesystem::path& out_dir, std::ostream& out,
                   std::ostream& err) {
  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto run = (*store)->get_run(run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  auto milestones = (*store)->list_milestones(run_id);
  auto usd = (*store)->total_usd(run_id);
  auto tokens = (*store)->total_tokens(run_id);
  auto defects = (*store)->list_defects(run_id);
  if (!milestones || !usd || !tokens || !defects) {
    err << "cannot read the run store\n";
    return 2;
  }

  auto plan = plan::parse_plan_file(run_directory(workspace, run_id) / "plan.yaml");

  // Milestone rows with their latest gate results.
  std::map<std::string, bool, std::less<>> milestone_gates_passed;
  Json milestone_rows = Json::array();
  for (const auto& milestone : *milestones) {
    Json row = milestone_json(milestone);
    auto gates = (*store)->latest_gate_results(run_id, milestone.milestone_id);
    row["gates"] = gates.has_value() ? gate_rows_json(*gates) : Json::array();
    bool passed = gates.has_value() && !gates->empty();
    if (gates.has_value()) {
      for (const auto& gate : *gates) {
        passed = passed && gate.passed;
      }
    } else {
      passed = false;
    }
    milestone_gates_passed[milestone.milestone_id] = passed;
    row["all_gates_passed"] = passed;
    milestone_rows.push_back(std::move(row));
  }

  // Requirement traceability (requirement -> milestones -> gate status).
  Json requirement_rows = Json::array();
  if (plan.has_value()) {
    std::map<std::string, std::vector<std::string>, std::less<>> by_requirement;
    for (const auto& spec : plan->milestones) {
      for (const auto& requirement : spec.requirements) {
        by_requirement[requirement].push_back(spec.id);
      }
    }
    for (const auto& [requirement, milestone_ids] : by_requirement) {
      bool done = true;
      bool gates_passed = true;
      Json ids = Json::array();
      for (const auto& id : milestone_ids) {
        ids.push_back(id);
        const auto state = std::find_if(
            milestones->begin(), milestones->end(),
            [&id](const store::MilestoneRecord& milestone) {
              return milestone.milestone_id == id;
            });
        done = done && state != milestones->end() && state->state == "Done";
        gates_passed = gates_passed && milestone_gates_passed[id];
      }
      requirement_rows.push_back(Json{{"requirement", requirement},
                                      {"milestones", std::move(ids)},
                                      {"milestones_done", done},
                                      {"gates_passed", gates_passed}});
    }
  }

  Json defect_rows = Json::array();
  for (const auto& defect : *defects) {
    defect_rows.push_back(defect_json(defect));
  }

  Json escalations = Json::array();
  const std::filesystem::path escalation_dir =
      run_directory(workspace, run_id) / "escalations";
  std::error_code ec;
  if (std::filesystem::is_directory(escalation_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(escalation_dir, ec)) {
      const auto path = entry.path() / "bundle.json";
      std::ifstream in(path);
      const Json bundle = Json::parse(in, nullptr, false);
      if (!bundle.is_discarded()) {
        escalations.push_back(Json{{"bundle", entry.path().filename().string()},
                                   {"reason", bundle.value("reason", "")}});
      }
    }
  }

  const Json document = Json{
      {"run", Json{{"id", run->run_id},
                   {"state", run->state},
                   {"plan_id", run->plan_id},
                   {"created_at", run->created_at},
                   {"cost", Json{{"usd", *usd}, {"tokens", *tokens}}}}},
      {"milestones", std::move(milestone_rows)},
      {"requirements", std::move(requirement_rows)},
      {"defects", std::move(defect_rows)},
      {"escalations", std::move(escalations)}};

  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    err << "cannot create the report directory: " << ec.message() << "\n";
    return 2;
  }
  {
    std::ofstream json_out(out_dir / "report.json", std::ios::binary);
    if (!json_out) {
      err << "cannot write report.json\n";
      return 2;
    }
    json_out << document.dump(2) << "\n";
  }
  {
    std::ofstream markdown(out_dir / "report.md", std::ios::binary);
    if (!markdown) {
      err << "cannot write report.md\n";
      return 2;
    }
    markdown << "# Run report: " << run->run_id << "\n\n";
    markdown << "- State: " << run->state << "\n";
    markdown << "- Plan: " << run->plan_id << "\n";
    markdown << "- Cost: $" << *usd << ", " << *tokens << " tokens\n\n";
    markdown << "## Milestones\n\n";
    markdown << "| id | state | iterations | gates |\n|---|---|---|---|\n";
    for (const auto& row : document["milestones"]) {
      markdown << "| " << row["id"].get<std::string>() << " | "
               << row["state"].get<std::string>() << " | "
               << row["iterations"].get<unsigned>() << " | "
               << (row["all_gates_passed"].get<bool>() ? "passed" : "not passed")
               << " |\n";
    }
    markdown << "\n## Requirement traceability\n\n";
    markdown << "| requirement | milestones | done | gates |\n|---|---|---|---|\n";
    for (const auto& row : document["requirements"]) {
      std::string ids;
      for (const auto& id : row["milestones"]) {
        if (!ids.empty()) {
          ids += ", ";
        }
        ids += id.get<std::string>();
      }
      markdown << "| " << row["requirement"].get<std::string>() << " | " << ids
               << " | " << (row["milestones_done"].get<bool>() ? "yes" : "no") << " | "
               << (row["gates_passed"].get<bool>() ? "passed" : "not passed") << " |\n";
    }
    markdown << "\n## Defects\n\n";
    if (document["defects"].empty()) {
      markdown << "None.\n";
    } else {
      markdown << "| id | milestone | status | severity |\n|---|---|---|---|\n";
      for (const auto& row : document["defects"]) {
        markdown << "| " << row["id"].get<std::string>() << " | "
                 << row["milestone_id"].get<std::string>() << " | "
                 << row["status"].get<std::string>() << " | "
                 << row["severity"].get<std::string>() << " |\n";
      }
    }
    markdown << "\n## Escalations\n\n";
    if (document["escalations"].empty()) {
      markdown << "None.\n";
    } else {
      for (const auto& row : document["escalations"]) {
        markdown << "- " << row["bundle"].get<std::string>() << ": "
                 << row["reason"].get<std::string>() << "\n";
      }
    }
  }

  out << Json{{"report_json", (out_dir / "report.json").string()},
              {"report_md", (out_dir / "report.md").string()}}
             .dump(2)
      << "\n";
  return 0;
}

int approve_command(const std::filesystem::path& workspace, const std::string& run_id,
                    const std::string& stage, std::ostream& out, std::ostream& err) {
  if (stage != "design") {
    err << "unknown approval stage '" << stage << "'; expected 'design'\n";
    return 1;
  }
  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  orch::StateMachine machine(**store, run_id);
  const int64_t now = store::unix_now();
  const auto applied = machine.apply(orch::RunEvent::Approved, {}, now);
  if (!applied) {
    err << applied.error().code << ": " << applied.error().message << "\n";
    return 1;
  }

  // Approved designs feed back into RAG (design section 6.2 step 7): the
  // package becomes a design document under <workspace>/designs, scoped to
  // the product, and an earlier approved design for the same plan becomes
  // superseded. Runs without a package (--skip-refinement) just approve.
  Json published;
  const std::filesystem::path design_path = run_directory(workspace, run_id) / "design.json";
  std::error_code design_ec;
  if (std::filesystem::exists(design_path, design_ec)) {
    auto package = orch::parse_design_package(read_file_text(design_path));
    if (!package) {
      err << package.error().code << ": " << package.error().message << "\n";
      return 1;
    }
    // The refiner stamps these; a hand-edited package may lack them.
    if (package->plan_id.empty() || package->product.empty()) {
      auto stored_plan = plan::parse_plan_file(run_directory(workspace, run_id) /
                                               "plan.yaml");
      if (stored_plan) {
        if (package->plan_id.empty()) {
          package->plan_id = stored_plan->plan_id;
        }
        if (package->product.empty()) {
          package->product = stored_plan->product;
        }
      }
    }
    auto path = orch::publish_design_package(workspace, run_id, *package, now);
    if (!path) {
      err << path.error().code << ": " << path.error().message << "\n";
      return 1;
    }
    published = path->string();
  }
  out << Json{{"run_id", run_id},
              {"state", std::string(orch::to_string(*applied))},
              {"design_document", std::move(published)}}
             .dump(2)
      << "\n";
  return 0;
}

int abort_command(const std::filesystem::path& workspace, const std::string& run_id,
                  const std::string& reason, std::ostream& out, std::ostream& err) {
  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto run = (*store)->get_run(run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  if (run->state != "Escalated") {
    err << "run " << run_id << " is in state " << run->state
        << "; a human can only abort an escalated run (design section 7.1)\n";
    return 1;
  }
  orch::StateMachine machine(**store, run_id);
  const auto applied =
      machine.apply(orch::RunEvent::HumanAborted, {}, store::unix_now());
  if (!applied) {
    err << applied.error().code << ": " << applied.error().message << "\n";
    return 1;
  }

  // An aborted run's designs are withdrawn from RAG (design section 6.2 step
  // 7): later runs must not build on a design that was abandoned.
  auto withdrawn = orch::withdraw_design_package(workspace, run_id);
  if (!withdrawn) {
    err << withdrawn.error().code << ": " << withdrawn.error().message << "\n";
    return 1;
  }
  out << Json{{"run_id", run_id},
              {"state", std::string(orch::to_string(*applied))},
              {"reason", reason},
              {"designs_withdrawn", *withdrawn}}
             .dump(2)
      << "\n";
  return 0;
}

int ingest_command(const IngestOptions& options, std::ostream& out,
                   std::ostream& err) {
  auto documents = rag::load_document_dir(options.docs_dir);
  if (!documents) {
    err << documents.error().code << ": " << documents.error().message << "\n";
    return 1;
  }

  std::unique_ptr<store::RunStore> store;
  if (!options.db_path.empty()) {
    auto opened = store::RunStore::open(options.db_path);
    if (!opened) {
      err << opened.error().code << ": " << opened.error().message << "\n";
      return 2;
    }
    store = std::move(*opened);
  }

  std::error_code ec;
  if (!options.out_dir.empty()) {
    std::filesystem::create_directories(options.out_dir, ec);
    if (ec) {
      err << "cannot create the output directory: " << ec.message() << "\n";
      return 2;
    }
  }

  Json ingested = Json::array();
  Json held = Json::array();
  for (const auto& document : *documents) {
    const auto findings = rag::scan_for_injected_instructions(document.text);
    if (!findings.empty()) {
      held.push_back(Json{{"doc_id", document.doc_id},
                          {"version", document.version},
                          {"pattern", findings.front().pattern},
                          {"line", findings.front().line},
                          {"reason", "content looks like an injected instruction; "
                                     "held for human review"}});
      continue;
    }

    const std::string digest = cf::sha256_hex(document.text);
    if (store != nullptr) {
      store::DocVersionRow row;
      row.doc_id = document.doc_id;
      row.version = document.version;
      row.sha256 = digest;
      row.precedence = rag::precedence_rank(document.type);
      row.ingested_at = store::unix_now();
      const auto recorded = store->upsert_doc_version(row);
      if (!recorded) {
        err << recorded.error().code << ": " << recorded.error().message << "\n";
        return 2;
      }
    }
    if (!options.out_dir.empty()) {
      const std::filesystem::path path =
          options.out_dir / (document.doc_id + "-" + document.version + ".md");
      std::ofstream file(path, std::ios::binary);
      if (!file) {
        err << "cannot write " << path.string() << "\n";
        return 2;
      }
      file << "---\n"
           << "doc_id: " << document.doc_id << "\n"
           << "version: \"" << document.version << "\"\n"
           << "type: " << rag::to_string(document.type) << "\n"
           << "owner: \"" << document.owner << "\"\n"
           << "classification: " << document.classification << "\n"
           << "scope: " << document.scope << "\n"
           << (document.scope_value.empty()
                   ? std::string()
                   : "scope_value: \"" + document.scope_value + "\"\n")
           << "status: " << document.status << "\n"
           << (document.license.empty()
                   ? std::string()
                   : "license: \"" + document.license + "\"\n")
           << "source: \"" << document.source << "\"\n"
           << "---\n\n"
           << document.text << "\n";
    }
    ingested.push_back(Json{{"doc_id", document.doc_id},
                            {"version", document.version},
                            {"type", std::string(rag::to_string(document.type))},
                            {"sha256", digest}});
  }

  out << Json{{"ingested", std::move(ingested)}, {"held", std::move(held)}}
             .dump(2)
      << "\n";
  return 0;
}

int execute_command(const ExecuteOptions& options, std::ostream& out, std::ostream& err,
                    cf::orch::SessionDriverFactory session_driver_factory) {
  auto store = store::RunStore::open(store_path(options.workspace, options.run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  const auto run = (*store)->get_run(options.run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  const std::filesystem::path directory =
      run_directory(options.workspace, options.run_id);
  auto plan = plan::parse_plan_file(directory / "plan.yaml");
  if (!plan) {
    err << plan.error().code << ": " << plan.error().message << "\n";
    return 1;
  }

  // Advance the run state (design section 7.1). Without --skip-refinement
  // the refinement pipeline runs first and the run stops at the human
  // approval gate (design section 6.2); `cf approve --stage design` releases
  // it, and the next execution runs with the refined plan.
  orch::StateMachine machine(**store, options.run_id);
  const auto advance = [&](orch::RunEvent event) {
    const auto applied = machine.apply(event, {}, store::unix_now());
    if (!applied) {
      err << applied.error().code << ": " << applied.error().message << "\n";
      return false;
    }
    return true;
  };

  if (!options.skip_refinement) {
    orch::RunState state = machine.current();
    if (state == orch::RunState::Created) {
      if (!advance(orch::RunEvent::Start)) return 1;
      state = machine.current();
    }
    if (state == orch::RunState::Ingesting) {
      if (!advance(orch::RunEvent::IndexReady)) return 1;
      state = machine.current();
    }
    if (state == orch::RunState::Refining) {
      auto package = refine_or_load(options, directory, *plan, out);
      if (!package) {
        err << package.error().code << ": " << package.error().message << "\n";
        return 1;
      }
      if (!advance(orch::RunEvent::DesignPackageReady)) return 1;
      state = machine.current();
    }
    if (state == orch::RunState::AwaitingDesignApproval) {
      out << "run " << options.run_id
          << " is AwaitingDesignApproval; review the design package and run:\n"
          << "  cf approve --run " << options.run_id << " --stage design\n";
      return 0;
    }
    if (state != orch::RunState::Executing) {
      err << "run " << options.run_id << " is in state "
          << std::string(orch::to_string(state)) << " and cannot be executed\n";
      return 1;
    }
  } else {
    for (int step = 0; step < 8; ++step) {
      const orch::RunState state = machine.current();
      if (state == orch::RunState::Executing) {
        break;
      }
      if (state == orch::RunState::Created || state == orch::RunState::Ingesting ||
          state == orch::RunState::Refining ||
          state == orch::RunState::AwaitingDesignApproval) {
        switch (state) {
          case orch::RunState::Created:
            if (!advance(orch::RunEvent::Start)) return 1;
            break;
          case orch::RunState::Ingesting:
            if (!advance(orch::RunEvent::IndexReady)) return 1;
            break;
          case orch::RunState::Refining:
            if (!advance(orch::RunEvent::DesignPackageReady)) return 1;
            break;
          case orch::RunState::AwaitingDesignApproval:
            if (!advance(orch::RunEvent::Approved)) return 1;
            break;
          default:
            break;
        }
        continue;
      }
      err << "run " << options.run_id << " is in state "
          << std::string(orch::to_string(state)) << " and cannot be executed\n";
      return 1;
    }
    if (machine.current() != orch::RunState::Executing) {
      err << "run " << options.run_id << " did not reach Executing\n";
      return 1;
    }
  }

  // A stored design package refines the plan that drives execution: the
  // scheduler reads the milestones' write sets and interfaces (design
  // sections 6.2, 6.13.2). The original plan stays for traceability.
  plan::Plan effective = *plan;
  const std::filesystem::path design_path = directory / "design.json";
  std::error_code design_ec;
  if (std::filesystem::exists(design_path, design_ec)) {
    auto package = orch::parse_design_package(read_file_text(design_path));
    if (!package) {
      err << package.error().code << ": " << package.error().message << "\n";
      return 1;
    }
    effective = orch::design_package_to_plan(*package, *plan);
  }

  if (options.repository_path.empty()) {
    err << "--repo is required to execute a run\n";
    return 1;
  }
  process::LocalProcessExecutor executor;
  auto repository = git::Repository::open(options.repository_path, executor);
  if (!repository) {
    err << repository.error().code << ": " << repository.error().message << "\n";
    return 2;
  }

  auto profiles = gates::ProfileRegistry::load_dir(options.profiles_dir);
  if (!profiles) {
    err << profiles.error().code << ": " << profiles.error().message << "\n";
    return 2;
  }
  auto profile = profiles->resolve(effective.target.language,
                                   effective.target.build_system,
                                   effective.target.test_framework);
  if (!profile) {
    err << profile.error().code << ": " << profile.error().message << "\n";
    return 1;
  }

  orch::RunProcessConfig config;
  config.run_id = options.run_id;
  config.workspace_root = options.workspace;
  config.integration_branch = "cf/" + options.run_id + "/integration";
  config.base_branch = "main";
  config.opencode_binary = options.opencode_binary;
  config.role_models = options.role_models;
  config.mcp_command = options.mcp_binary;
  config.docs_dir = options.docs_dir;
  if (!options.patterns_dir.empty() &&
      std::filesystem::is_directory(options.patterns_dir)) {
    config.patterns_dir = options.patterns_dir;
  }
  config.profile = **profile;
  config.profile_language = effective.target.language;
  config.profile_build_system = effective.target.build_system;
  config.profile_test_framework = effective.target.test_framework;
  config.sandbox_image = options.sandbox_image;
  config.orchestrator.escalation_dir = (directory / "escalations").string();
  if (effective.budget.max_usd_total.has_value()) {
    config.orchestrator.max_usd_total = effective.budget.max_usd_total;
  }
  if (effective.budget.max_tokens_total.has_value()) {
    config.orchestrator.max_tokens_total = effective.budget.max_tokens_total;
  }

  // GitHub flow: the App identity (preferred) or a static token.
  std::unique_ptr<llm::CprTransport> default_github_transport;
  std::unique_ptr<github::AppAuth> app_auth;
  if (!options.github_owner.empty() && !options.github_repo.empty()) {
    llm::HttpTransport* transport = options.github_transport;
    if (transport == nullptr) {
      default_github_transport = std::make_unique<llm::CprTransport>();
      transport = default_github_transport.get();
    }
    config.github_transport = transport;
    config.github_api_base = options.github_api_base;
    config.github_owner = options.github_owner;
    config.github_repo = options.github_repo;
    config.github_token = options.github_token;
    if (!options.github_app_id.empty() && !options.github_private_key_file.empty()) {
      std::ifstream key_file(options.github_private_key_file, std::ios::binary);
      if (!key_file) {
        err << "cannot read the GitHub App private key file: "
            << options.github_private_key_file << "\n";
        return 2;
      }
      std::ostringstream key_buffer;
      key_buffer << key_file.rdbuf();
      github::AppAuthOptions auth_options;
      auth_options.app_id = options.github_app_id;
      auth_options.installation_id = options.github_installation_id;
      auth_options.private_key_pem = key_buffer.str();
      auth_options.api_base = options.github_api_base;
      app_auth = std::make_unique<github::AppAuth>(auth_options, *transport);
      github::AppAuth* auth = app_auth.get();
      config.github_token_provider = [auth]() { return auth->installation_token(); };
    }
  }

  orch::RunProcess process(**store, effective, config, *repository, executor);
  if (session_driver_factory) {
    process.set_session_driver_factory(std::move(session_driver_factory));
  }
  auto final_state = process.execute(store::unix_now());
  if (!final_state) {
    err << final_state.error().code << ": " << final_state.error().message << "\n";
    return 1;
  }
  out << Json{{"run_id", options.run_id},
              {"state", std::string(orch::to_string(*final_state))}}
             .dump(2)
      << "\n";
  return *final_state == orch::RunState::Done ? 0 : 1;
}

}  // namespace cf::cli
