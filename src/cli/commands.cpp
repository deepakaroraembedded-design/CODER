#include "cf/cli/commands.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iomanip>
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

#include "cf/codeintel/backend.hpp"
#include "cf/codeintel/treesitter_backend.hpp"
#ifdef CF_HAS_CLANG
#include "cf/codeintel/clang_backend.hpp"
#endif
#include "cf/common/hash.hpp"
#include "cf/common/process.hpp"
#include "cf/gates/profile.hpp"
#include "cf/git/repository.hpp"
#include "cf/github/app_auth.hpp"
#include "cf/github/client.hpp"
#include "cf/github/codeowners.hpp"
#include "cf/github/rulesets.hpp"
#include "cf/llm/client.hpp"
#include "cf/orch/approval_deadline.hpp"
#include "cf/orch/dispatcher.hpp"
#include "cf/orch/metrics.hpp"
#include "cf/orch/recovery.hpp"
#include "cf/orch/run_process.hpp"
#include "cf/orch/run_summary.hpp"
#include "cf/orch/state_machine.hpp"
#include "cf/plan/plan.hpp"
#include "cf/rag/catalog.hpp"
#include "cf/rag/embedder.hpp"
#include "cf/rag/injection_scan.hpp"
#include "cf/rag/loader.hpp"
#include "cf/rag/retriever.hpp"
#include "cf/refmirror/manifest.hpp"
#include "cf/refmirror/mirror.hpp"
#include "cf/refmirror/versions.hpp"
#include "cf/sandbox/docker.hpp"
#include "cf/store/run_store.hpp"
#include "cf/telemetry/logging.hpp"
#include "cf/telemetry/metrics.hpp"

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

// Locate the company guardrail rules. CF_RULES_DIR wins; otherwise look next
// to the resolved profile (<repo>/profiles/x.yaml -> <repo>/rules) and then in
// the current directory. The directory is mounted into gate/agent sandboxes.
std::string resolve_rules_dir(const std::filesystem::path& profile_path) {
  std::vector<std::filesystem::path> candidates;
  if (const char* environment = std::getenv("CF_RULES_DIR");
      environment != nullptr && *environment != '\0') {
    candidates.emplace_back(environment);
  }
  if (!profile_path.empty()) {
    candidates.push_back(profile_path.parent_path().parent_path() / "rules");
  }
  candidates.push_back(std::filesystem::current_path() / "rules");
  for (const auto& candidate : candidates) {
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(candidate, error);
    if (!error && std::filesystem::is_directory(absolute)) {
      return absolute.string();
    }
  }
  return {};
}

std::string read_file_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

struct ReferenceDocs {
  bool configured = false;
  std::filesystem::path docs_dir;
  std::filesystem::path versions_file;
};

// Mirrors the approved reference documentation for this run and pins the
// versions the target project's dependency files select (design section
// 6.3.1). A version mismatch is reported and the set is skipped, never
// silently replaced; on_missing_version: fail aborts instead.
Result<ReferenceDocs> prepare_reference_docs(const ExecuteOptions& options,
                                             const std::filesystem::path& run_dir) {
  ReferenceDocs result;
  if (!options.reference_docs_manifest.has_value() ||
      options.reference_docs_manifest->empty()) {
    return result;
  }
  result.configured = true;
  std::error_code ec;
  std::filesystem::path manifest_path = *options.reference_docs_manifest;
  if (!manifest_path.is_absolute()) {
    manifest_path = std::filesystem::absolute(manifest_path, ec);
    if (ec) {
      return fail("REFDOCS_MANIFEST_INVALID",
                  "cannot resolve the reference-docs manifest: " + ec.message());
    }
  }
  auto manifest = refmirror::load_manifest_file(manifest_path);
  if (!manifest) {
    return tl::make_unexpected(manifest.error());
  }

  std::map<std::string, std::string, std::less<>> versions;
  const std::string version_source =
      options.reference_docs_version_source.value_or("auto");
  if (version_source != "none" && !options.repository_path.empty()) {
    auto dependencies = refmirror::dependency_versions(options.repository_path);
    if (!dependencies) {
      return tl::make_unexpected(dependencies.error());
    }
    versions = std::move(*dependencies);
  }

  refmirror::VersionReport version_report;
  auto matched = refmirror::match_versions(*manifest, versions, version_report);
  if (!version_report.skipped.empty()) {
    const std::string on_missing =
        options.reference_docs_on_missing_version.value_or("report");
    std::string detail;
    for (const auto& skipped : version_report.skipped) {
      if (!detail.empty()) {
        detail += ", ";
      }
      detail += skipped.id + " (mirrored " + skipped.mirrored_version +
                ", project pins " + skipped.pinned_version + ")";
    }
    if (on_missing == "fail") {
      return fail("REFDOCS_VERSION_MISSING",
                  "no mirrored documentation for the pinned version: " + detail);
    }
    spdlog::warn("reference docs skipped for a version mismatch: {}", detail);
  }

  result.docs_dir = run_dir / "reference-docs";
  const std::filesystem::path mirror_report = result.docs_dir / "report.json";
  if (!std::filesystem::exists(mirror_report, ec)) {
    refmirror::MirrorOptions mirror_options;
    mirror_options.out_dir = result.docs_dir;
    llm::CprTransport transport;
    auto report = refmirror::run_mirror(matched, mirror_options, transport);
    if (!report) {
      return tl::make_unexpected(report.error());
    }
    if (!report->ok()) {
      spdlog::warn("reference mirror completed with held or failed sets: {}",
                   report->to_json());
    }
  }

  result.versions_file = run_dir / "reference-versions.json";
  Json skipped = Json::array();
  for (const auto& entry : version_report.skipped) {
    skipped.push_back(Json{{"id", entry.id},
                           {"mirrored_version", entry.mirrored_version},
                           {"pinned_version", entry.pinned_version}});
  }
  const Json document = Json{{"manifest", manifest_path.string()},
                             {"versions", version_report.pinned},
                             {"skipped", std::move(skipped)}};
  std::filesystem::create_directories(run_dir, ec);
  std::ofstream versions_out(result.versions_file, std::ios::binary);
  if (!versions_out) {
    return fail("REFDOCS_REPORT_FAILED",
                "cannot write " + result.versions_file.string());
  }
  versions_out << document.dump(2) << "\n";
  return result;
}

void apply_plan_approvals(const plan::Plan& plan,
                          orch::ApprovalDeadlines& deadlines) {
  if (plan.approvals.design_approval_business_days.has_value()) {
    deadlines.design_approval_business_days =
        *plan.approvals.design_approval_business_days;
  }
  if (plan.approvals.escalation_business_days.has_value()) {
    deadlines.escalation_business_days =
        *plan.approvals.escalation_business_days;
  }
  if (plan.approvals.waiver_business_days.has_value()) {
    deadlines.waiver_business_days = *plan.approvals.waiver_business_days;
  }
}

void apply_stored_approvals(const store::RunRecord& run,
                            orch::ApprovalDeadlines& deadlines) {
  const Json config = Json::parse(run.config_json, nullptr, false);
  if (!config.is_object()) {
    return;
  }
  const auto approvals = config.find("approvals");
  if (approvals == config.end() || !approvals->is_object()) {
    return;
  }
  deadlines.design_approval_business_days = approvals->value(
      "design_business_days", deadlines.design_approval_business_days);
  deadlines.escalation_business_days = approvals->value(
      "escalation_business_days", deadlines.escalation_business_days);
  deadlines.waiver_business_days =
      approvals->value("waiver_business_days", deadlines.waiver_business_days);
}

Status store_approvals(const store::RunRecord& run,
                       const orch::ApprovalDeadlines& deadlines,
                       store::RunStore& store) {
  Json config = Json::parse(run.config_json, nullptr, false);
  if (!config.is_object()) {
    config = Json::object();
  }
  config["approvals"] =
      Json{{"design_business_days", deadlines.design_approval_business_days},
           {"escalation_business_days", deadlines.escalation_business_days},
           {"waiver_business_days", deadlines.waiver_business_days}};
  return store.update_run_config(run.run_id, config.dump());
}

// Bridges the planner model over cf-llm (design section 6.2): the refiner
// makes direct model calls, not opencode agent turns.
class LlmDesignModel final : public orch::DesignModel {
 public:
  LlmDesignModel(llm::FireworksClient& client, std::string model, std::string run_id)
      : client_(client), model_(std::move(model)), run_id_(std::move(run_id)) {}

  Result<std::string> complete(std::string_view prompt) override {
    llm::ChatRequest request;
    request.model = model_;
    request.context.run_id = run_id_;
    request.context.role = "planner";
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
  std::string run_id_;
};

// Runs the refinement pipeline for a run in Refining: an existing design
// package is reused (so a retry after a crash never plans twice), otherwise
// the planner model produces one, validated and stored as design.json plus a
// human-readable design.md (design section 6.2).
Result<orch::DesignPackage> refine_or_load(
    const ExecuteOptions& options, const std::filesystem::path& run_dir,
    const std::filesystem::path& reference_docs_dir, const plan::Plan& plan,
    store::RunStore* store, std::ostream& out) {
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
    // Encrypted prompt/response archive and the cost ledger (design
    // sections 6.11, 14). The key is CF_ARCHIVE_KEY; without it the archive
    // is skipped, while the ledger is always recorded.
    client_options.archive_dir = run_dir / "llm";
    llm::UsageCallback on_call;
    if (store != nullptr) {
      on_call = [store](const llm::CallRecord& record) {
        store::LlmCallRecord row;
        row.run_id = record.run_id;
        row.role = record.role;
        row.model = record.model;
        row.prompt_tokens = record.usage.prompt_tokens;
        row.completion_tokens = record.usage.completion_tokens;
        row.cached_tokens = record.usage.cached_tokens;
        row.usd = record.usd;
        row.latency_ms = record.latency_ms;
        row.prompt_ref = record.prompt_ref;
        row.response_ref = record.response_ref;
        row.at = store::unix_now();
        (void)store->record_llm_call(row);
      };
    }
    client = std::make_unique<llm::FireworksClient>(
        client_options, std::make_unique<llm::CprTransport>(), on_call);
    model = std::make_unique<LlmDesignModel>(*client, model_id, options.run_id);
    active = model.get();
  }

  // Company documents plus approved design packages from earlier runs
  // (design section 6.2 step 7); the applicability filter keeps only the
  // ones scoped to this plan's product.
  const std::filesystem::path designs_dir = options.workspace / "designs";
  rag::RetrieverOptions retriever_options;
  if (store != nullptr) {
    // RAG latency samples for the run metrics (design section 14).
    retriever_options.on_search_latency_ms =
        [store, run_id = options.run_id](std::int64_t latency_ms) {
          (void)store->record_rag_latency(run_id, latency_ms, store::unix_now());
        };
  }
  if (options.rag_index.has_value()) {
    retriever_options.hnsw = *options.rag_index == "hnswlib";
    retriever_options.faiss = *options.rag_index == "faiss";
    retriever_options.faiss_ivfpq = *options.rag_index == "faiss-ivfpq";
  }
  if (options.rag_bm25_weight.has_value()) {
    retriever_options.bm25_weight = *options.rag_bm25_weight;
  }
  if (options.rag_vector_weight.has_value()) {
    retriever_options.vector_weight = *options.rag_vector_weight;
  }
  if (options.rag_precedence.has_value()) {
    auto types = rag::doc_types_from_names(*options.rag_precedence);
    if (!types) {
      return fail("ORCH_REFINE_FAILED", types.error().message);
    }
    retriever_options.precedence = std::move(*types);
  }

  std::unique_ptr<rag::InMemoryRetriever> retriever;
  std::shared_ptr<llm::FireworksClient> embedding_client;
  if (!options.docs_dir.empty() || !reference_docs_dir.empty() ||
      std::filesystem::is_directory(designs_dir)) {
    retriever =
        std::make_unique<rag::InMemoryRetriever>(retriever_options);
    if (!options.embeddings_model.empty()) {
      const char* key = std::getenv(options.embeddings_api_key_env.c_str());
      if (key == nullptr || *key == '\0') {
        spdlog::warn("{} is not set; hybrid retrieval uses BM25 only",
                     options.embeddings_api_key_env);
      } else {
        llm::ClientOptions embedding_options;
        embedding_options.base_url = options.embeddings_base_url;
        embedding_options.api_key = key;
        llm::UsageCallback on_call;
        if (store != nullptr) {
          on_call = [store](const llm::CallRecord& record) {
            store::LlmCallRecord row;
            row.run_id = record.run_id;
            row.role = record.role;
            row.model = record.model;
            row.prompt_tokens = record.usage.prompt_tokens;
            row.completion_tokens = record.usage.completion_tokens;
            row.cached_tokens = record.usage.cached_tokens;
            row.usd = record.usd;
            row.latency_ms = record.latency_ms;
            row.at = store::unix_now();
            (void)store->record_llm_call(row);
          };
        }
        embedding_client = std::make_shared<llm::FireworksClient>(
            embedding_options, std::make_unique<llm::CprTransport>(), on_call);
        retriever->set_embedder(std::make_shared<rag::FireworksEmbedder>(
            *embedding_client, options.embeddings_model, options.run_id));
      }
    }
    for (const auto& directory : {std::filesystem::path(options.docs_dir),
                                  reference_docs_dir, designs_dir}) {
      std::error_code directory_ec;
      if (directory.empty() ||
          !std::filesystem::is_directory(directory, directory_ec)) {
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
  if (options.rag_top_k.has_value()) {
    refiner_options.context_hits = *options.rag_top_k;
  }
  const std::filesystem::path feedback_path = run_dir / "design-feedback.txt";
  std::error_code feedback_ec;
  if (std::filesystem::is_regular_file(feedback_path, feedback_ec)) {
    refiner_options.feedback = read_file_text(feedback_path);
    while (!refiner_options.feedback.empty() &&
           (refiner_options.feedback.back() == '\n' ||
            refiner_options.feedback.back() == '\r')) {
      refiner_options.feedback.pop_back();
    }
  }
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
  std::filesystem::remove(feedback_path, feedback_ec);
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
  auto waiver_requests = (*store)->list_waiver_requests(run_id);
  if (!milestones || !operations || !transitions || !usd || !tokens ||
      !locks || !waiver_requests) {
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

  Json request_rows = Json::array();
  for (const auto& request : *waiver_requests) {
    request_rows.push_back(Json{{"id", request.id},
                                {"milestone_id", request.milestone_id},
                                {"rule", request.rule},
                                {"scope", request.scope},
                                {"requested_by", request.requested_by},
                                {"status", request.status},
                                {"deadline_at", request.deadline_at}});
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
                       {"waiver_requests", std::move(request_rows)},
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
  if (sandbox::docker_available()) {
    options.kill_leftovers = [run_id]() {
      return sandbox::remove_run_containers("docker", run_id);
    };
  }
  auto report = recovery.resume(run_id, dispatcher, options, store::unix_now());
  if (!report) {
    err << report.error().code << ": " << report.error().message << "\n";
    return 1;
  }
  // The resume command does not go on to execute the run, so it must not
  // keep the lease (a second orchestrator would be locked out until the TTL).
  const auto released = (*store)->release_lease(run_id, holder, report->epoch);
  if (!released) {
    err << released.error().code << ": " << released.error().message << "\n";
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
  auto plan = plan::parse_plan_file(run_directory(workspace, run_id) / "plan.yaml");
  auto summary = orch::build_run_summary(
      **store, run_id, plan.has_value() ? &*plan : nullptr,
      run_directory(workspace, run_id));
  if (!summary) {
    err << summary.error().code << ": " << summary.error().message << "\n";
    return summary.error().code == store::error_codes::kNotFound ? 1 : 2;
  }

  (void)(*store)->expire_waiver_requests(run_id, store::unix_now());
  auto metric_samples =
      orch::collect_run_metrics(**store, run_id, run_directory(workspace, run_id));
  if (!metric_samples) {
    err << metric_samples.error().code << ": " << metric_samples.error().message
        << "\n";
    return 2;
  }
  Json metrics = Json::array();
  std::vector<telemetry::MetricPoint> metric_points;
  metric_points.reserve(metric_samples->size());
  for (const auto& sample : *metric_samples) {
    metrics.push_back(Json{{"name", sample.name},
                           {"labels", sample.labels},
                           {"value", sample.value}});
    const bool counter = sample.name == "cf_llm_tokens_total" ||
                         sample.name == "cf_llm_usd_total" ||
                         sample.name == "cf_escalations_total";
    metric_points.push_back(telemetry::MetricPoint{
        sample.name, sample.labels, sample.value,
        counter ? telemetry::MetricKind::Counter
                : telemetry::MetricKind::Gauge});
  }
  Json otlp = Json::object();
  const char* otlp_endpoint = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
  if (otlp_endpoint != nullptr && *otlp_endpoint != '\0') {
    llm::CprTransport transport;
    auto exported =
        telemetry::export_otlp_metrics(metric_points, otlp_endpoint, transport);
    if (exported) {
      otlp = Json{{"endpoint", otlp_endpoint}, {"exported", true}};
    } else {
      otlp = Json{{"endpoint", otlp_endpoint},
                  {"exported", false},
                  {"error", exported.error().message}};
      spdlog::warn("OTLP metrics export failed: {}", exported.error().message);
    }
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
      {"run", Json{{"id", summary->run.run_id},
                   {"state", summary->run.state},
                   {"plan_id", summary->run.plan_id},
                   {"created_at", summary->run.created_at},
                   {"cost",
                    Json{{"usd", summary->usd}, {"tokens", summary->tokens}}}}},
      {"milestones", summary->milestones},
      {"requirements", summary->requirements},
      {"defects", summary->defects},
      {"waivers", summary->waivers},
      {"waiver_requests", summary->waiver_requests},
      {"agent_usage",
       Json{{"by_role", summary->agent_usage_by_role},
            {"by_milestone", summary->agent_usage_by_milestone}}},
      {"metrics", std::move(metrics)},
      {"otlp_metrics", std::move(otlp)},
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
    markdown << "# Run report: " << summary->run.run_id << "\n\n";
    markdown << "- State: " << summary->run.state << "\n";
    markdown << "- Plan: " << summary->run.plan_id << "\n";
    markdown << "- Cost: $" << summary->usd << ", " << summary->tokens
             << " tokens\n\n";
    markdown << orch::render_run_summary_markdown(*summary);
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
  auto run = (*store)->get_run(run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  orch::ApprovalDeadlines deadlines;
  auto plan = plan::parse_plan_file(run_directory(workspace, run_id) / "plan.yaml");
  if (plan) {
    apply_plan_approvals(*plan, deadlines);
  }
  apply_stored_approvals(*run, deadlines);
  const int64_t now = store::unix_now();
  const auto expired =
      orch::expire_waiting_run(**store, run_id, deadlines, now);
  if (!expired) {
    err << expired.error().code << ": " << expired.error().message << "\n";
    return 1;
  }
  if (expired->has_value()) {
    out << Json{{"run_id", run_id},
                {"state", "Aborted"},
                {"reason", **expired}}
               .dump(2)
        << "\n";
    return 1;
  }
  (void)(*store)->expire_waiver_requests(run_id, store::unix_now());
  orch::StateMachine machine(**store, run_id);
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

int request_design_changes_command(const std::filesystem::path& workspace,
                                   const std::string& run_id,
                                   const std::string& reason, std::ostream& out,
                                   std::ostream& err) {
  const auto blank = [](const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
  };
  if (blank(reason)) {
    err << "--request-changes needs --reason <text>; explain what must change\n";
    return 1;
  }
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
  if (run->state != "AwaitingDesignApproval") {
    err << "run " << run_id << " is in state " << run->state
        << "; changes can only be requested at AwaitingDesignApproval\n";
    return 1;
  }

  orch::ApprovalDeadlines deadlines;
  auto plan = plan::parse_plan_file(run_directory(workspace, run_id) / "plan.yaml");
  if (plan) {
    apply_plan_approvals(*plan, deadlines);
  }
  apply_stored_approvals(*run, deadlines);
  const auto expired = orch::expire_waiting_run(
      **store, run_id, deadlines, store::unix_now());
  if (!expired) {
    err << expired.error().code << ": " << expired.error().message << "\n";
    return 1;
  }
  if (expired->has_value()) {
    out << Json{{"run_id", run_id},
                {"state", "Aborted"},
                {"reason", **expired}}
               .dump(2)
        << "\n";
    return 1;
  }

  // Keep the rejected package for audit and remove it from the place the
  // refiner reuses, so the next execution actually refines again.
  const std::filesystem::path run_dir = run_directory(workspace, run_id);
  const store::UnixTime now = store::unix_now();
  Json archived = Json::array();
  std::error_code ec;
  for (const char* name : {"design.json", "design.md"}) {
    const std::filesystem::path source = run_dir / name;
    if (!std::filesystem::is_regular_file(source, ec)) {
      continue;
    }
    const std::string stem = std::filesystem::path(name).stem().string();
    const std::filesystem::path destination =
        run_dir / (stem + ".rejected-" + std::to_string(now) +
                   std::filesystem::path(name).extension().string());
    std::filesystem::rename(source, destination, ec);
    if (ec) {
      err << "cannot archive " << source.string() << ": " << ec.message()
          << "\n";
      return 1;
    }
    archived.push_back(destination.filename().string());
  }

  const std::filesystem::path feedback_path = run_dir / "design-feedback.txt";
  std::ofstream feedback(feedback_path, std::ios::binary | std::ios::trunc);
  if (!feedback) {
    err << "cannot write " << feedback_path.string() << "\n";
    return 2;
  }
  feedback << reason << "\n";
  feedback.close();

  orch::StateMachine machine(**store, run_id);
  const auto applied = machine.apply(orch::RunEvent::ChangesRequested, {}, now);
  if (!applied) {
    err << applied.error().code << ": " << applied.error().message << "\n";
    return 1;
  }
  out << Json{{"run_id", run_id},
              {"state", std::string(orch::to_string(*applied))},
              {"archived", std::move(archived)},
              {"feedback_file", feedback_path.string()}}
             .dump(2)
      << "\n";
  return 0;
}

int approve_waiver_command(const std::filesystem::path& workspace,
                           const std::string& run_id,
                           const WaiverOptions& options, std::ostream& out,
                           std::ostream& err) {
  const auto blank = [](const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
  };
  if (blank(options.rule)) {
    err << "--waiver needs a rule (the gate or check name)\n";
    return 1;
  }
  if (blank(options.scope)) {
    err << "--waiver needs --scope <file:line> naming what is waived\n";
    return 1;
  }
  if (blank(options.justification)) {
    err << "--waiver needs --justification <text>; a waiver is only valid "
           "with a written reason (design section 10.2)\n";
    return 1;
  }
  if (blank(options.approved_by)) {
    err << "--waiver needs --approved-by <person> (or USER in the "
           "environment) so the report names who approved it\n";
    return 1;
  }

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
  if (!options.milestone_id.empty()) {
    auto milestone = (*store)->get_milestone(run_id, options.milestone_id);
    if (!milestone) {
      err << milestone.error().code << ": " << milestone.error().message << "\n";
      return 1;
    }
  }

  store::WaiverRecord waiver;
  waiver.run_id = run_id;
  waiver.milestone_id = options.milestone_id;
  waiver.rule = options.rule;
  waiver.scope = options.scope;
  waiver.justification = options.justification;
  waiver.approved_by = options.approved_by;
  waiver.at = store::unix_now();
  auto recorded = (*store)->record_waiver(waiver);
  if (!recorded) {
    err << recorded.error().code << ": " << recorded.error().message << "\n";
    return recorded.error().code == store::error_codes::kNotFound ? 1 : 2;
  }
  // The human decision settles the matching pending agent requests.
  (void)(*store)->decide_waiver_requests(run_id, options.milestone_id,
                                         options.rule, options.scope, "APPROVED",
                                         recorded->at);

  // A waiver of a gate failure also releases an escalated milestone back to
  // Gating, so the next `cf run --execute` re-runs the gates with the waiver
  // in force. With no --milestone, only escalated milestones whose latest
  // gate failure names the rule are released.
  orch::StateMachine machine(**store, run_id);
  Json resolved = Json::array();
  const auto release_milestone = [&](const std::string& milestone_id) -> Status {
    auto applied =
        machine.apply(milestone_id, orch::MilestoneEvent::WaiverGranted, waiver.at);
    if (!applied) {
      return tl::make_unexpected(applied.error());
    }
    resolved.push_back(milestone_id);
    return {};
  };
  if (!options.milestone_id.empty()) {
    auto milestone = (*store)->get_milestone(run_id, options.milestone_id);
    if (milestone && milestone->state == "Escalated") {
      const auto released = release_milestone(options.milestone_id);
      if (!released) {
        err << released.error().code << ": " << released.error().message << "\n";
        return 1;
      }
    }
  } else if (run->state == "Escalated") {
    auto milestones = (*store)->list_milestones(run_id);
    if (!milestones) {
      err << milestones.error().code << ": " << milestones.error().message << "\n";
      return 1;
    }
    for (const auto& milestone : *milestones) {
      if (milestone.state != "Escalated") {
        continue;
      }
      bool matches = true;
      auto gates = (*store)->latest_gate_results(run_id, milestone.milestone_id);
      if (gates.has_value() && !gates->empty()) {
        matches = false;
        for (const auto& row : *gates) {
          if (!row.passed && row.gate == options.rule) {
            matches = true;
            break;
          }
        }
      }
      if (!matches) {
        continue;
      }
      const auto released = release_milestone(milestone.milestone_id);
      if (!released) {
        err << released.error().code << ": " << released.error().message << "\n";
        return 1;
      }
    }
  }

  std::string state = run->state;
  if (state == "Escalated" && !resolved.empty()) {
    auto applied = machine.apply(orch::RunEvent::HumanResolved, {}, waiver.at);
    if (!applied) {
      err << applied.error().code << ": " << applied.error().message << "\n";
      return 1;
    }
    state = std::string(orch::to_string(*applied));
  }

  out << Json{{"run_id", run_id},
              {"state", state},
              {"resolved_milestones", std::move(resolved)},
              {"waiver", Json{{"id", recorded->waiver_id},
                              {"milestone_id", recorded->milestone_id},
                              {"rule", recorded->rule},
                              {"scope", recorded->scope},
                              {"justification", recorded->justification},
                              {"approved_by", recorded->approved_by},
                              {"at", recorded->at}}}}
             .dump(2)
      << "\n";
  return 0;
}

Result<store::UnixTime> parse_github_time(std::string_view text) {
  if (text.empty()) {
    return fail("CLI_TIME_INVALID", "the pull request has no close timestamp");
  }
  std::tm tm{};
  std::istringstream in{std::string(text)};
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (in.fail()) {
    return fail("CLI_TIME_INVALID",
                "cannot parse the GitHub timestamp '" + std::string(text) + "'");
  }
  return static_cast<store::UnixTime>(::timegm(&tm));
}

int reminder_one_run(const ReminderOptions& options, std::ostream& out,
                     std::ostream& err) {
  auto store = store::RunStore::open(store_path(options.workspace, options.run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto run = (*store)->get_run(options.run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  std::string state_key;
  std::string state_label;
  std::vector<std::uint32_t> thresholds;
  if (run->state == "AwaitingDesignApproval") {
    state_key = "design_approval";
    state_label = "design approval";
    thresholds = {2, 4};
  } else if (run->state == "Escalated") {
    state_key = "escalation";
    state_label = "an escalation";
    thresholds = {1, 2};
  }

  auto waiver_requests = (*store)->list_waiver_requests(options.run_id);
  if (!waiver_requests) {
    err << waiver_requests.error().code << ": " << waiver_requests.error().message
        << "\n";
    return 1;
  }
  std::vector<const store::WaiverRequestRecord*> pending_requests;
  for (const auto& request : *waiver_requests) {
    if (request.status == "PENDING") {
      pending_requests.push_back(&request);
    }
  }
  if (state_key.empty() && pending_requests.empty()) {
    out << Json{{"run_id", options.run_id},
                {"state", run->state},
                {"reason", "not_waiting"},
                {"posted", Json::array()}}
               .dump(2)
        << "\n";
    return 0;
  }
  if (options.github_owner.empty() || options.github_repo.empty()) {
    err << "remind needs the GitHub owner and repo (cf.yaml or flags)\n";
    return 1;
  }

  auto operations =
      (*store)->list_operations(options.run_id, {store::op_state::kConfirmed});
  if (!operations) {
    err << operations.error().code << ": " << operations.error().message << "\n";
    return 1;
  }
  int64_t pull_request = 0;
  for (const auto& operation : *operations) {
    if (operation.kind != "pr_create") {
      continue;
    }
    const Json result = Json::parse(operation.result_json, nullptr, false);
    if (!result.is_discarded()) {
      pull_request = result.value("number", int64_t{0});
    }
  }
  if (pull_request == 0) {
    out << Json{{"run_id", options.run_id},
                {"state", run->state},
                {"reason", "no_pull_request"},
                {"posted", Json::array()}}
               .dump(2)
        << "\n";
    return 0;
  }

  auto transitions = (*store)->list_transitions(options.run_id);
  if (!transitions) {
    err << transitions.error().code << ": " << transitions.error().message << "\n";
    return 1;
  }
  store::UnixTime waiting_since = run->created_at;
  for (const auto& transition : *transitions) {
    if (transition.to_state == run->state) {
      waiting_since = transition.at;
    }
  }
  const store::UnixTime now = store::unix_now();
  const std::uint32_t elapsed =
      orch::business_days_between(waiting_since, now);

  std::unique_ptr<llm::CprTransport> default_transport;
  llm::HttpTransport* transport = options.github_transport;
  if (transport == nullptr) {
    default_transport = std::make_unique<llm::CprTransport>();
    transport = default_transport.get();
  }
  std::unique_ptr<github::AppAuth> app_auth;
  github::ClientOptions client_options;
  client_options.api_base = options.github_api_base;
  if (!options.github_app_id.empty() &&
      !options.github_private_key_file.empty()) {
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
    client_options.token_provider = [auth]() { return auth->installation_token(); };
  } else {
    client_options.token = options.github_token;
  }
  github::Client client(client_options, *transport);

  auto comments = client.list_issue_comments(options.github_owner,
                                              options.github_repo, pull_request);
  if (!comments) {
    err << comments.error().code << ": " << comments.error().message << "\n";
    return 1;
  }

  std::vector<std::string> mentions;
  for (const char* path : {".github/CODEOWNERS", "CODEOWNERS",
                           "docs/CODEOWNERS"}) {
    auto file = client.get_file(options.github_owner, options.github_repo, path);
    if (!file) {
      continue;
    }
    const auto owners = github::parse_codeowners(*file);
    for (const auto& user : owners.users) {
      mentions.push_back("@" + user);
    }
    break;
  }
  std::string mention_text;
  for (const auto& mention : mentions) {
    if (!mention_text.empty()) {
      mention_text += " ";
    }
    mention_text += mention;
  }

  Json posted = Json::array();
  Json skipped = Json::array();
  for (const std::uint32_t threshold : thresholds) {
    if (state_key.empty() || threshold > elapsed) {
      continue;
    }
    const std::string marker =
        "<!-- cf-reminder:" + state_key + ":" + std::to_string(threshold) + " -->";
    bool exists = false;
    for (const auto& comment : *comments) {
      if (comment.body.find(marker) != std::string::npos) {
        exists = true;
        break;
      }
    }
    if (exists) {
      skipped.push_back(threshold);
      continue;
    }
    std::string body =
        "Reminder: this run has been waiting for " + state_label + " for " +
        std::to_string(threshold) + " business day(s).";
    if (!mention_text.empty()) {
      body += " " + mention_text;
    }
    body += "\n\n" + marker;
    auto created = client.create_comment(options.github_owner, options.github_repo,
                                         pull_request, body);
    if (!created) {
      err << created.error().code << ": " << created.error().message << "\n";
      return 1;
    }
    posted.push_back(threshold);
  }

  Json waiver_posted = Json::array();
  Json waiver_skipped = Json::array();
  for (const store::WaiverRequestRecord* request : pending_requests) {
    const std::uint32_t request_elapsed =
        orch::business_days_between(request->requested_at, now);
    if (request_elapsed < 1) {
      continue;
    }
    const std::string marker =
        "<!-- cf-reminder:waiver:" + request->id + ":1 -->";
    bool exists = false;
    for (const auto& comment : *comments) {
      if (comment.body.find(marker) != std::string::npos) {
        exists = true;
        break;
      }
    }
    if (exists) {
      waiver_skipped.push_back(request->id);
      continue;
    }
    std::string body = "Reminder: waiver request " + request->id + " (" +
                       request->rule + " @ " + request->scope +
                       ") is still waiting for a human decision.";
    if (!mention_text.empty()) {
      body += " " + mention_text;
    }
    body += "\n\n" + marker;
    auto created = client.create_comment(options.github_owner, options.github_repo,
                                         pull_request, body);
    if (!created) {
      err << created.error().code << ": " << created.error().message << "\n";
      return 1;
    }
    waiver_posted.push_back(request->id);
  }

  out << Json{{"run_id", options.run_id},
              {"state", run->state},
              {"pull_request", pull_request},
              {"elapsed_business_days", elapsed},
              {"posted", std::move(posted)},
              {"skipped", std::move(skipped)},
              {"waiver_requests_posted", std::move(waiver_posted)},
              {"waiver_requests_skipped", std::move(waiver_skipped)},
              {"reason", "ok"}}
             .dump(2)
      << "\n";
  return 0;
}

int cleanup_one_run(const CleanupOptions& options, std::ostream& out,
                    std::ostream& err) {
  auto store = store::RunStore::open(store_path(options.workspace, options.run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto run = (*store)->get_run(options.run_id);
  if (!run) {
    err << run.error().code << ": " << run.error().message << "\n";
    return 1;
  }
  if (options.github_owner.empty() || options.github_repo.empty()) {
    err << "cleanup needs the GitHub owner and repo (cf.yaml or flags)\n";
    return 1;
  }
  if (options.repository_path.empty()) {
    err << "cleanup needs --repo (a local checkout with the remote)\n";
    return 1;
  }

  // The pull request number is the confirmed pr_create operation (design
  // section 10.4.4); no PR means there are no cf/ branches to retire.
  auto operations =
      (*store)->list_operations(options.run_id, {store::op_state::kConfirmed});
  if (!operations) {
    err << operations.error().code << ": " << operations.error().message << "\n";
    return 1;
  }
  int64_t pull_request = 0;
  for (const auto& operation : *operations) {
    if (operation.kind != "pr_create") {
      continue;
    }
    const Json result = Json::parse(operation.result_json, nullptr, false);
    if (!result.is_discarded()) {
      pull_request = result.value("number", int64_t{0});
    }
  }
  if (pull_request == 0) {
    out << Json{{"run_id", options.run_id},
                {"reason", "no_pull_request"},
                {"deleted", Json::array()}}
               .dump(2)
        << "\n";
    return 0;
  }

  std::unique_ptr<llm::CprTransport> default_transport;
  llm::HttpTransport* transport = options.github_transport;
  if (transport == nullptr) {
    default_transport = std::make_unique<llm::CprTransport>();
    transport = default_transport.get();
  }
  std::unique_ptr<github::AppAuth> app_auth;
  github::ClientOptions client_options;
  client_options.api_base = options.github_api_base;
  if (!options.github_app_id.empty() &&
      !options.github_private_key_file.empty()) {
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
    client_options.token_provider = [auth]() { return auth->installation_token(); };
  } else {
    client_options.token = options.github_token;
  }
  github::Client client(client_options, *transport);

  auto pull = client.get_pull_request(options.github_owner, options.github_repo,
                                      pull_request);
  if (!pull) {
    err << pull.error().code << ": " << pull.error().message << "\n";
    return 1;
  }
  if (pull->state != "closed") {
    out << Json{{"run_id", options.run_id},
                {"pull_request", pull_request},
                {"reason", "pull_request_open"},
                {"deleted", Json::array()}}
               .dump(2)
        << "\n";
    return 0;
  }
  const std::string closed_at =
      pull->merged_at.empty() ? pull->closed_at : pull->merged_at;
  auto closed = parse_github_time(closed_at);
  if (!closed) {
    err << closed.error().code << ": " << closed.error().message << "\n";
    return 1;
  }
  const store::UnixTime delete_after =
      *closed + static_cast<store::UnixTime>(options.retention_days) * 86400;
  const store::UnixTime now = store::unix_now();
  if (now < delete_after) {
    out << Json{{"run_id", options.run_id},
                {"pull_request", pull_request},
                {"reason", "retained"},
                {"delete_after", delete_after},
                {"deleted", Json::array()}}
               .dump(2)
        << "\n";
    return 0;
  }

  process::LocalProcessExecutor executor;
  auto repository = git::Repository::open(options.repository_path, executor);
  if (!repository) {
    err << repository.error().code << ": " << repository.error().message << "\n";
    return 2;
  }
  const std::string pattern = "refs/heads/cf/" + options.run_id + "/*";
  auto listed =
      repository->command({"ls-remote", "--heads", options.github_remote, pattern});
  if (!listed) {
    err << listed.error().code << ": " << listed.error().message << "\n";
    return 1;
  }
  std::vector<std::string> branches;
  std::istringstream lines(*listed);
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    std::string ref = line.substr(tab + 1);
    const std::string prefix = "refs/heads/";
    if (ref.rfind(prefix, 0) != 0) {
      continue;
    }
    ref = ref.substr(prefix.size());
    const std::string run_prefix = "cf/" + options.run_id + "/";
    if (ref.rfind(run_prefix, 0) == 0) {
      branches.push_back(ref);
    }
  }
  std::sort(branches.begin(), branches.end());

  Json deleted = Json::array();
  Json failed = Json::array();
  for (const auto& branch : branches) {
    auto status = client.delete_branch(options.github_owner, options.github_repo,
                                       branch);
    if (status) {
      deleted.push_back(branch);
    } else {
      failed.push_back(Json{{"branch", branch},
                            {"code", status.error().code},
                            {"message", status.error().message}});
    }
  }
  out << Json{{"run_id", options.run_id},
              {"pull_request", pull_request},
              {"reason", failed.empty() ? "deleted" : "partial"},
              {"deleted", std::move(deleted)},
              {"failed", std::move(failed)}}
             .dump(2)
      << "\n";
  return failed.empty() ? 0 : 1;
}

int cleanup_command(const CleanupOptions& options, std::ostream& out,
                    std::ostream& err) {
  if (!options.all) {
    return cleanup_one_run(options, out, err);
  }
  const std::filesystem::path runs_dir = options.workspace / "runs";
  std::error_code ec;
  if (!std::filesystem::is_directory(runs_dir, ec)) {
    out << Json{{"runs", Json::array()}}.dump(2) << "\n";
    return 0;
  }
  Json results = Json::array();
  int worst = 0;
  for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string run_id = entry.path().filename().string();
    if (!std::filesystem::is_regular_file(entry.path() / "run.db", ec)) {
      continue;
    }
    CleanupOptions one = options;
    one.all = false;
    one.run_id = run_id;
    std::ostringstream run_out;
    std::ostringstream run_err;
    const int code = cleanup_one_run(one, run_out, run_err);
    Json row = Json::parse(run_out.str(), nullptr, false);
    if (!row.is_object()) {
      row = Json{{"reason", "error"}, {"detail", run_err.str()}};
    }
    row["exit_code"] = code;
    if (code > worst) {
      worst = code;
    }
    results.push_back(std::move(row));
  }
  out << Json{{"runs", std::move(results)}}.dump(2) << "\n";
  return worst;
}

int rulesets_command(const RulesetCommandOptions& options, std::ostream& out,
                     std::ostream& err) {
  github::RulesetOptions ruleset_options;
  ruleset_options.required_checks = options.required_checks;
  const Json protected_rules = github::protected_branch_ruleset(ruleset_options);
  const Json cf_rules = github::cf_branch_ruleset(ruleset_options);
  if (options.out_dir.empty()) {
    out << Json{{"protected_branches", protected_rules},
                {"cf_branches", cf_rules}}
               .dump(2)
        << "\n";
    return 0;
  }
  std::error_code ec;
  std::filesystem::create_directories(options.out_dir, ec);
  if (ec) {
    err << "cannot create the ruleset directory: " << ec.message() << "\n";
    return 2;
  }
  const auto write = [&](const std::filesystem::path& path, const Json& document) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }
    file << document.dump(2) << "\n";
    return true;
  };
  const auto protected_path = options.out_dir / "protected-branches.json";
  const auto cf_path = options.out_dir / "cf-branches.json";
  if (!write(protected_path, protected_rules) || !write(cf_path, cf_rules)) {
    err << "cannot write the ruleset payloads\n";
    return 2;
  }
  out << Json{{"protected_branches", protected_path.string()},
              {"cf_branches", cf_path.string()}}
             .dump(2)
      << "\n";
  return 0;
}

int reminder_command(const ReminderOptions& options, std::ostream& out,
                     std::ostream& err) {
  if (!options.all) {
    return reminder_one_run(options, out, err);
  }
  const std::filesystem::path runs_dir = options.workspace / "runs";
  std::error_code ec;
  if (!std::filesystem::is_directory(runs_dir, ec)) {
    out << Json{{"runs", Json::array()}}.dump(2) << "\n";
    return 0;
  }
  Json results = Json::array();
  int worst = 0;
  for (const auto& entry : std::filesystem::directory_iterator(runs_dir, ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string run_id = entry.path().filename().string();
    if (!std::filesystem::is_regular_file(entry.path() / "run.db", ec)) {
      continue;
    }
    ReminderOptions one = options;
    one.all = false;
    one.run_id = run_id;
    std::ostringstream run_out;
    std::ostringstream run_err;
    const int code = reminder_one_run(one, run_out, run_err);
    Json row = Json::parse(run_out.str(), nullptr, false);
    if (!row.is_object()) {
      row = Json{{"reason", "error"}, {"detail", run_err.str()}};
    }
    row["exit_code"] = code;
    if (code > worst) {
      worst = code;
    }
    results.push_back(std::move(row));
  }
  out << Json{{"runs", std::move(results)}}.dump(2) << "\n";
  return worst;
}

int refuse_waiver_request_command(const std::filesystem::path& workspace,
                                  const std::string& run_id,
                                  const std::string& request_id,
                                  const std::string& reason, std::ostream& out,
                                  std::ostream& err) {
  auto store = store::RunStore::open(store_path(workspace, run_id));
  if (!store) {
    err << store.error().code << ": " << store.error().message << "\n";
    return 2;
  }
  auto request = (*store)->get_waiver_request(request_id);
  if (!request) {
    err << request.error().code << ": " << request.error().message << "\n";
    return 1;
  }
  if (request->run_id != run_id) {
    err << "waiver request " << request_id << " belongs to run "
        << request->run_id << ", not " << run_id << "\n";
    return 1;
  }
  if (request->status != "PENDING") {
    err << "waiver request " << request_id << " is " << request->status
        << ", not PENDING\n";
    return 1;
  }
  const auto refused =
      (*store)->set_waiver_request_status(request_id, "REFUSED");
  if (!refused) {
    err << refused.error().code << ": " << refused.error().message << "\n";
    return 1;
  }
  out << Json{{"run_id", run_id},
              {"request_id", request_id},
              {"status", "REFUSED"},
              {"reason", reason}}
             .dump(2)
      << "\n";
  return 0;
}

int replay_command(const ReplayOptions& options, std::ostream& out,
                   std::ostream& err) {
  const std::filesystem::path archive =
      run_directory(options.workspace, options.run_id) / "llm" /
      (options.call_id + ".cfar");
  auto call = llm::read_archived_call(archive);
  if (!call) {
    err << call.error().code << ": " << call.error().message << "\n";
    return 1;
  }
  Json request = call->request;
  if (!request.is_object()) {
    err << "LLM_ARCHIVE_INVALID: the archived request is not an object\n";
    return 1;
  }
  if (!options.model.empty()) {
    request["model"] = options.model;
  }
  const char* key = std::getenv(options.api_key_env.c_str());
  if (key == nullptr || *key == '\0') {
    err << options.api_key_env << " is not set\n";
    return 2;
  }
  std::unique_ptr<llm::CprTransport> default_transport;
  llm::HttpTransport* transport = options.transport;
  if (transport == nullptr) {
    default_transport = std::make_unique<llm::CprTransport>();
    transport = default_transport.get();
  }
  llm::HttpRequest http;
  http.url = (options.base_url.empty() ? call->base_url : options.base_url) +
             call->path;
  http.headers = {{"Authorization", "Bearer " + std::string(key)},
                  {"Content-Type", "application/json"},
                  {"Accept", "application/json"}};
  http.body = request.dump();
  auto response = transport->post(http);
  if (!response) {
    err << response.error().code << ": " << response.error().message << "\n";
    return 1;
  }
  const Json parsed = Json::parse(response->body, nullptr, false);
  out << Json{{"call_id", call->call_id},
              {"model", request.value("model", call->model)},
              {"status", response->status},
              {"response", parsed.is_discarded() ? Json(response->body) : parsed}}
             .dump(2)
      << "\n";
  return response->status >= 200 && response->status < 300 ? 0 : 1;
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

Result<std::vector<rag::Document>> load_converted_documents(
    const std::filesystem::path& dir, process::ProcessExecutor& executor,
    Json& held) {
  std::vector<rag::Document> documents;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string extension = entry.path().extension().string();
    if (extension != ".pdf" && extension != ".docx") {
      continue;
    }
    const std::filesystem::path sidecar =
        entry.path().string() + ".meta.yaml";
    if (!std::filesystem::is_regular_file(sidecar, ec)) {
      held.push_back(Json{{"doc_id", entry.path().filename().string()},
                          {"reason", extension +
                                         " input needs a " +
                                         entry.path().filename().string() +
                                         ".meta.yaml metadata sidecar"}});
      continue;
    }
    process::ProcessRequest request;
    if (extension == ".pdf") {
      request.argv = {"pdftotext", "-layout", entry.path().string(), "-"};
    } else {
      request.argv = {"pandoc", "-f", "docx", "-t", "markdown",
                      entry.path().string()};
    }
    request.timeout = std::chrono::minutes(2);
    request.output_limit_bytes = 16u << 20;
    const process::ProcessResult converted = executor.run(request);
    if (!converted.launched || converted.exit_code != 0 || converted.timed_out) {
      held.push_back(Json{{"doc_id", entry.path().filename().string()},
                          {"reason", "cannot convert " + extension + " input: " +
                                         (converted.error.empty()
                                              ? converted.stderr_text
                                              : converted.error)}});
      continue;
    }
    const std::string text = "---\n" + read_file_text(sidecar) +
                             "\n---\n\n" + converted.stdout_text + "\n";
    auto document = rag::load_document_string(text, entry.path().string());
    if (!document) {
      held.push_back(Json{{"doc_id", entry.path().filename().string()},
                          {"reason", "invalid metadata sidecar: " +
                                         document.error().message}});
      continue;
    }
    documents.push_back(std::move(*document));
  }
  return documents;
}

int ingest_command(const IngestOptions& options, std::ostream& out,
                   std::ostream& err) {
  auto documents = rag::load_document_dir(options.docs_dir);
  if (!documents) {
    err << documents.error().code << ": " << documents.error().message << "\n";
    return 1;
  }
  Json ingested = Json::array();
  Json held = Json::array();
  process::LocalProcessExecutor local_executor;
  process::ProcessExecutor& executor =
      options.executor == nullptr ? local_executor : *options.executor;
  auto converted = load_converted_documents(options.docs_dir, executor, held);
  if (!converted) {
    err << converted.error().code << ": " << converted.error().message << "\n";
    return 1;
  }
  documents->insert(documents->end(),
                    std::make_move_iterator(converted->begin()),
                    std::make_move_iterator(converted->end()));

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
  telemetry::ScopedContext log_context(options.run_id, "", "orchestrator");
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
  // Offline reference documentation (design section 6.3.1): mirror the
  // approved sets once per run and pin the project's dependency versions.
  // Terminal runs have nothing left to index.
  ReferenceDocs reference_docs;
  if (run->state != "Done" && run->state != "Aborted") {
    auto prepared = prepare_reference_docs(options, directory);
    if (!prepared) {
      err << prepared.error().code << ": " << prepared.error().message << "\n";
      return 1;
    }
    reference_docs = std::move(*prepared);
  }

  // Deadlines for states that wait for people (design section 12.1): cf.yaml
  // supplies defaults, the plan overrides them, and the resolved values are
  // stored so `cf approve` sees the same deadline.
  orch::ApprovalDeadlines deadlines;
  if (options.design_approval_business_days.has_value()) {
    deadlines.design_approval_business_days =
        *options.design_approval_business_days;
  }
  if (options.escalation_business_days.has_value()) {
    deadlines.escalation_business_days =
        *options.escalation_business_days;
  }
  if (options.waiver_business_days.has_value()) {
    deadlines.waiver_business_days = *options.waiver_business_days;
  }
  apply_plan_approvals(*plan, deadlines);
  const auto stored = store_approvals(*run, deadlines, **store);
  if (!stored) {
    err << stored.error().code << ": " << stored.error().message << "\n";
    return 1;
  }
  (void)(*store)->expire_waiver_requests(options.run_id, store::unix_now());
  const auto expired = orch::expire_waiting_run(
      **store, options.run_id, deadlines, store::unix_now());
  if (!expired) {
    err << expired.error().code << ": " << expired.error().message << "\n";
    return 1;
  }
  if (expired->has_value()) {
    out << Json{{"run_id", options.run_id},
                {"state", "Aborted"},
                {"reason", **expired}}
               .dump(2)
        << "\n";
    return 0;
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
      auto package = refine_or_load(options, directory, reference_docs.docs_dir,
                                    *plan, &**store, out);
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
  // Every CF commit is signed with the configured SSH key (design section
  // 10.4.2). The key is read by git on each commit.
  if (!options.github_signing_key_file.empty()) {
    auto signing = repository->set_signing_key(options.github_signing_key_file);
    if (!signing) {
      err << signing.error().code << ": " << signing.error().message << "\n";
      return 2;
    }
  }

  auto profiles = gates::ProfileRegistry::load_dir(options.profiles_dir);
  if (!profiles) {
    err << profiles.error().code << ": " << profiles.error().message << "\n";
    return 2;
  }
  const gates::Profile* selected_profile = nullptr;
  if (options.profile_id.has_value() && *options.profile_id != "auto") {
    auto found = profiles->find(*options.profile_id);
    if (!found) {
      err << found.error().code << ": " << found.error().message << "\n";
      return 1;
    }
    selected_profile = *found;
    // FR-18 still applies: the pinned profile must declare the plan's exact
    // language/build-system/test-framework combination.
    if (selected_profile->languages.find(effective.target.language) ==
            selected_profile->languages.end() ||
        selected_profile->build_systems.find(effective.target.build_system) ==
            selected_profile->build_systems.end() ||
        selected_profile->test_frameworks.find(effective.target.test_framework) ==
            selected_profile->test_frameworks.end()) {
      err << "PLAN_NO_TOOLCHAIN_PROFILE: pinned profile '"
          << *options.profile_id << "' does not declare "
          << effective.target.language << "/" << effective.target.build_system
          << "/" << effective.target.test_framework << "\n";
      return 1;
    }
  } else {
    auto profile = profiles->resolve(effective.target.language,
                                     effective.target.build_system,
                                     effective.target.test_framework);
    if (!profile) {
      err << profile.error().code << ": " << profile.error().message << "\n";
      return 1;
    }
    selected_profile = *profile;
  }

  orch::RunProcessConfig config;
  config.run_id = options.run_id;
  config.waiver_business_days = deadlines.waiver_business_days;
  config.workspace_root = options.workspace;
  config.integration_branch = "cf/" + options.run_id + "/integration";
  config.base_branch = options.base_branch;
  config.opencode_binary = options.opencode_binary;
  config.role_models = options.role_models;
  config.role_options = options.role_options;
  config.mcp_command = options.mcp_binary;
  config.docs_dir = options.docs_dir;
  if (!options.patterns_dir.empty() &&
      std::filesystem::is_directory(options.patterns_dir)) {
    config.patterns_dir = options.patterns_dir;
  }
  config.rag_index = options.rag_index;
  config.rag_top_k = options.rag_top_k;
  config.rag_bm25_weight = options.rag_bm25_weight;
  config.rag_vector_weight = options.rag_vector_weight;
  config.rag_precedence = options.rag_precedence;
  config.embeddings_model = options.embeddings_model;
  config.embeddings_base_url = options.embeddings_base_url;
  config.embeddings_api_key_env = options.embeddings_api_key_env;
  config.reference_docs_dir = reference_docs.docs_dir.string();
  config.reference_versions_file = reference_docs.versions_file.string();
  config.profile = *selected_profile;
  // cf.yaml stage-3 overrides win over the profile defaults (design section
  // 15). The agent-side run_checks copy keeps the profile file's defaults.
  if (options.coverage_line.has_value()) {
    config.profile.coverage.line = *options.coverage_line;
  }
  if (options.coverage_branch.has_value()) {
    config.profile.coverage.branch = *options.coverage_branch;
  }
  if (options.mutation_min_score.has_value()) {
    config.profile.mutation.min_score = *options.mutation_min_score;
  }
  if (options.mutation_enabled.has_value() && !*options.mutation_enabled) {
    config.profile.mutation.steps.clear();
  }
  if (config.profile.coverage.steps.empty() &&
      (options.coverage_line.has_value() || options.coverage_branch.has_value())) {
    spdlog::warn("gates.coverage overrides are set but the profile declares no "
                 "coverage steps");
  }
  if (config.profile.mutation.steps.empty() &&
      options.mutation_enabled.value_or(false)) {
    spdlog::warn("gates.mutation is enabled but the profile declares no "
                 "mutation steps");
  }
  config.profile_path = selected_profile->source_path.string();
  config.rules_dir = resolve_rules_dir(config.profile_path);
  config.profile_language = effective.target.language;
  config.profile_build_system = effective.target.build_system;
  config.profile_test_framework = effective.target.test_framework;
  config.sandbox_image = options.sandbox_image;
  config.agent_sandbox_image = options.agent_sandbox_image;
  config.agent_sandbox_binary = options.agent_sandbox_binary;
  config.agent_sandbox_namespaces = options.agent_sandbox_namespaces;
  config.ns_agent_binary = options.ns_agent_binary;
  config.ns_mounts = options.ns_mounts;
  config.ns_path_prefixes = options.ns_path_prefixes;
  config.ns_env = options.ns_env;
  // Code intelligence (design section 6.5): the available back ends; the
  // run indexes when one matches the plan's target language.
  codeintel::BackendRegistry codeintel_backends;
#ifdef CF_HAS_CLANG
  // Clang's AST is the primary C/C++ back end; the tree-sitter factories stay
  // linked as the fallback for projects Clang cannot parse.
  codeintel_backends.add(
      codeintel::make_clang_backend("c", {".c"}));
  codeintel_backends.add(codeintel::make_clang_backend(
      "cpp", {".cc", ".cpp", ".cxx", ".c++", ".cp"}));
#else
  codeintel_backends.add(codeintel::make_c_backend());
  codeintel_backends.add(codeintel::make_cpp_backend());
#endif
  codeintel_backends.add(codeintel::make_python_backend());
  codeintel_backends.add(codeintel::make_go_backend());
  codeintel_backends.add(codeintel::make_typescript_backend());
  codeintel_backends.add(codeintel::make_javascript_backend());
  codeintel_backends.add(codeintel::make_java_backend());
  config.codeintel_backends = &codeintel_backends;
  config.orchestrator.escalation_dir = (directory / "escalations").string();
  // Budget: the plan's block wins over cf.yaml defaults (design Q5).
  if (effective.budget.max_usd_total.has_value()) {
    config.orchestrator.max_usd_total = effective.budget.max_usd_total;
  } else if (options.budget_usd.has_value()) {
    config.orchestrator.max_usd_total = options.budget_usd;
  }
  if (effective.budget.max_tokens_total.has_value()) {
    config.orchestrator.max_tokens_total = effective.budget.max_tokens_total;
  } else if (options.budget_tokens.has_value()) {
    config.orchestrator.max_tokens_total = options.budget_tokens;
  }
  if (effective.budget.max_wall_clock_hours.has_value()) {
    config.orchestrator.max_wall_clock_hours =
        effective.budget.max_wall_clock_hours;
  } else if (options.budget_wall_clock_hours.has_value()) {
    config.orchestrator.max_wall_clock_hours =
        options.budget_wall_clock_hours;
  }
  if (options.max_iterations_per_milestone.has_value()) {
    config.orchestrator.loop_limits.max_iterations_per_milestone =
        *options.max_iterations_per_milestone;
  }
  if (options.max_attempts_per_defect.has_value()) {
    config.orchestrator.loop_limits.max_attempts_per_defect =
        *options.max_attempts_per_defect;
  }
  if (options.review_diff_threshold_lines.has_value()) {
    config.full_review_threshold = *options.review_diff_threshold_lines;
  }
  if (options.review_blocking_severities.has_value()) {
    config.review_blocking_severities = *options.review_blocking_severities;
  }
  if (options.sandbox_cpus.has_value()) {
    config.gate_cpus = *options.sandbox_cpus;
  }
  if (options.sandbox_memory_gb.has_value()) {
    config.gate_memory_mb =
        static_cast<std::uint64_t>(*options.sandbox_memory_gb * 1024.0);
  }
  if (options.gate_timeout_min.has_value()) {
    config.step_timeout = std::chrono::minutes(*options.gate_timeout_min);
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
    // With a signing key configured, every commit pushed must be signed
    // (design section 10.4.6 check 7).
    config.github_pre_push.require_signatures =
        !options.github_signing_key_file.empty();
    config.github_pre_push.base_branch = options.base_branch;
    if (options.github_branch_prefix.has_value()) {
      config.github_pre_push.branch_prefix = *options.github_branch_prefix;
    }
    if (options.github_protected_paths.has_value()) {
      // The standard protected paths stay protected; the config adds to
      // them (design section 10.4.6).
      for (const auto& path : *options.github_protected_paths) {
        auto& paths = config.github_pre_push.protected_paths;
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
          paths.push_back(path);
        }
      }
    }
    if (options.github_max_file_mb.has_value()) {
      config.github_pre_push.max_file_mb = *options.github_max_file_mb;
    }
    if (options.github_push_on.has_value()) {
      config.github_push_on = *options.github_push_on;
    }
    if (options.github_pr_draft_until_done.has_value()) {
      config.github_pr_draft_until_done = *options.github_pr_draft_until_done;
    }
    if (options.github_pr_labels.has_value()) {
      config.github_pr_labels = *options.github_pr_labels;
    }
    if (options.github_pr_request_codeowners.has_value()) {
      config.github_pr_request_codeowners =
          *options.github_pr_request_codeowners;
    }
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
