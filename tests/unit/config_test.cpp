// Unit tests for the cf.yaml configuration loader (design section 15).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/cli/config.hpp"

namespace {

using cf::cli::ExecuteOptions;
using cf::cli::FrameworkConfig;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_config_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override { dir_ = make_temp_dir(); }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path write_config(const std::string& text) {
    const auto path = dir_ / "cf.yaml";
    std::ofstream out(path);
    out << text;
    return path;
  }

  std::filesystem::path dir_;
};

const char* kFull = R"yaml(
run:
  workspace: /srv/cf/work
  repo: /srv/checkouts/product-y
  base_branch: main
github:
  app_id: "12345"
  installation_id: "67890"
  private_key_ref: /srv/keys/github-app.pem
  signing_key_ref: /srv/keys/cf-signing
  branch_prefix: "cf/"
  protected_paths: [".github/**", "CODEOWNERS", "deploy/**"]
  max_file_mb: 7
  push_on: [run_done, escalation, run_done]
  branch_retention_days: 14
  pr:
    draft_until_done: false
    labels: [cf-generated, automated]
    request_codeowners: false
providers:
  fireworks:
    base_url: https://api.fireworks.ai/inference/v1
    api_key_env: FIREWORKS_API_KEY
  local-vllm:
    base_url: http://vllm-host:8000/v1
    api_key_env: VLLM_API_KEY
models:
  default_provider: fireworks
  roles:
    planner:  { family: deepseek, fireworks: "accounts/fireworks/models/deepseek-v4p1", temperature: 0.2 }
    coder:    { family: deepseek, fireworks: "accounts/fireworks/models/deepseek-v4p1-flash", temperature: 0.1, max_output_tokens: 8192 }
    reviewer: { family: kimi,     fireworks: "accounts/fireworks/models/kimi-k3", temperature: 0.0 }
    qa:       { family: glm,      fireworks: "accounts/fireworks/models/glm-5p3" }
  embeddings:
    fireworks: { model: "accounts/fireworks/models/embed", dim: 768, index: hosted }
budget:
  max_usd_total: 150
  max_tokens_total: 60000000
  max_iterations_per_milestone: 12
  max_attempts_per_defect: 4
  max_wall_clock_hours: 24
approvals:
  design_approval: 5d
  escalation: 3d
  waiver: 2d
reference_docs:
  manifest: /srv/refs/manifest.yaml
  index_version: pinned_at_run_start
  version_source: auto
  on_missing_version: report
rag:
  index: hnswlib
  top_k: 12
  hybrid: { bm25_weight: 0.4, vector_weight: 0.6 }
  precedence: [design, policy, requirements, architecture, guideline, pattern, external_reference]
gates:
  profiles_dir: /srv/cf/profiles
  profile: c-cpp
  coverage: { line: 0.9, branch: 0.8, scope: all }
  mutation: { enabled: true, min_score: 0.7 }
review:
  diff_threshold_lines: 250
  blocking_severities: [blocker, major, minor]
sandbox:
  cpu: 8
  memory_gb: 16
  job_timeout_min: 30
  network: deny
)yaml";

TEST_F(ConfigTest, LoadsTheFullSchema) {
  auto config = cf::cli::load_config(write_config(kFull));
  ASSERT_TRUE(config.has_value()) << config.error().message;

  EXPECT_EQ(*config->run.workspace, "/srv/cf/work");
  EXPECT_EQ(*config->run.repo, "/srv/checkouts/product-y");
  EXPECT_EQ(*config->run.base_branch, "main");
  EXPECT_EQ(*config->github.app_id, "12345");
  EXPECT_EQ(*config->github.installation_id, "67890");
  EXPECT_EQ(*config->github.private_key_ref, "/srv/keys/github-app.pem");
  EXPECT_EQ(*config->github.signing_key_ref, "/srv/keys/cf-signing");
  EXPECT_EQ(*config->github.branch_prefix, "cf/");
  EXPECT_EQ(*config->github.protected_paths,
            (std::vector<std::string>{".github/**", "CODEOWNERS", "deploy/**"}));
  EXPECT_EQ(*config->github.max_file_mb, 7u);
  EXPECT_EQ(*config->github.push_on,
            (std::vector<std::string>{"run_done", "escalation"}));
  EXPECT_EQ(*config->github.branch_retention_days, 14u);
  EXPECT_FALSE(*config->github.pr.draft_until_done);
  EXPECT_EQ(*config->github.pr.labels,
            (std::vector<std::string>{"cf-generated", "automated"}));
  EXPECT_FALSE(*config->github.pr.request_codeowners);

  ASSERT_EQ(config->providers.size(), 2u);
  EXPECT_EQ(config->providers.at("local-vllm").base_url, "http://vllm-host:8000/v1");
  EXPECT_EQ(config->providers.at("local-vllm").api_key_env, "VLLM_API_KEY");

  EXPECT_EQ(config->models.default_provider, "fireworks");
  ASSERT_EQ(config->models.roles.size(), 4u);
  const auto& coder = config->models.roles.at("coder");
  EXPECT_EQ(coder.family, "deepseek");
  EXPECT_EQ(coder.ids.at("fireworks"),
            "accounts/fireworks/models/deepseek-v4p1-flash");
  EXPECT_DOUBLE_EQ(*coder.temperature, 0.1);
  EXPECT_EQ(*coder.max_output_tokens, 8192u);
  EXPECT_FALSE(config->models.roles.at("qa").temperature.has_value());
  ASSERT_EQ(config->models.embeddings.size(), 1u);
  EXPECT_EQ(config->models.embeddings.at("fireworks").model,
            "accounts/fireworks/models/embed");
  EXPECT_EQ(*config->models.embeddings.at("fireworks").dim, 768u);
  EXPECT_EQ(*config->models.embeddings.at("fireworks").index, "hosted");

  EXPECT_DOUBLE_EQ(*config->budget.max_usd_total, 150.0);
  EXPECT_EQ(*config->budget.max_tokens_total, 60000000u);
  EXPECT_EQ(*config->budget.max_iterations_per_milestone, 12u);
  EXPECT_EQ(*config->budget.max_attempts_per_defect, 4u);
  EXPECT_DOUBLE_EQ(*config->budget.max_wall_clock_hours, 24.0);
  EXPECT_EQ(*config->approvals.design_approval_business_days, 5u);
  EXPECT_EQ(*config->approvals.escalation_business_days, 3u);
  EXPECT_EQ(*config->approvals.waiver_business_days, 2u);
  EXPECT_EQ(*config->reference_docs.manifest, "/srv/refs/manifest.yaml");
  EXPECT_EQ(*config->reference_docs.index_version, "pinned_at_run_start");
  EXPECT_EQ(*config->reference_docs.version_source, "auto");
  EXPECT_EQ(*config->reference_docs.on_missing_version, "report");
  EXPECT_EQ(*config->rag.index, "hnswlib");
  EXPECT_EQ(*config->rag.top_k, 12u);
  EXPECT_DOUBLE_EQ(*config->rag.bm25_weight, 0.4);
  EXPECT_DOUBLE_EQ(*config->rag.vector_weight, 0.6);
  ASSERT_EQ(config->rag.precedence->size(), 7u);
  EXPECT_EQ(config->rag.precedence->front(), "design");
  EXPECT_EQ(config->gates.profiles_dir, "/srv/cf/profiles");
  EXPECT_EQ(*config->gates.profile, "c-cpp");
  EXPECT_DOUBLE_EQ(*config->gates.coverage.line, 0.9);
  EXPECT_DOUBLE_EQ(*config->gates.coverage.branch, 0.8);
  EXPECT_EQ(*config->gates.coverage.scope, "all");
  EXPECT_TRUE(*config->gates.mutation.enabled);
  EXPECT_DOUBLE_EQ(*config->gates.mutation.min_score, 0.7);
  EXPECT_EQ(*config->review.diff_threshold_lines, 250u);
  EXPECT_EQ(*config->review.blocking_severities,
            (std::vector<std::string>{"blocker", "major", "minor"}));
  EXPECT_DOUBLE_EQ(*config->sandbox.cpu, 8.0);
  EXPECT_DOUBLE_EQ(*config->sandbox.memory_gb, 16.0);
  EXPECT_EQ(*config->sandbox.job_timeout_min, 30u);
  EXPECT_EQ(*config->sandbox.network, "deny");
}

TEST_F(ConfigTest, EmptyAndMissingFiles) {
  auto empty = cf::cli::load_config(write_config(""));
  ASSERT_TRUE(empty.has_value()) << empty.error().message;
  EXPECT_TRUE(empty->providers.empty());

  auto missing = cf::cli::load_config(dir_ / "nope.yaml");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, cf::cli::config_error_codes::kOpenFailed);
}

TEST_F(ConfigTest, UnknownAndUnsupportedKeysAreRejected) {
  auto unknown = cf::cli::load_config(write_config("run: {}\nbogus: 1\n"));
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, cf::cli::config_error_codes::kInvalid);
  EXPECT_NE(unknown.error().message.find("bogus"), std::string::npos);

  auto nested = cf::cli::load_config(write_config("run: {workspac: /tmp}\n"));
  ASSERT_FALSE(nested.has_value());
  EXPECT_NE(nested.error().message.find("workspac"), std::string::npos);

  // A documented knob this build does not consume yet says so.
  auto unsupported = cf::cli::load_config(
      write_config("sandbox: {bogus: 1}\n"));
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_NE(unsupported.error().message.find("not supported"),
            std::string::npos);

  auto push_event =
      cf::cli::load_config(write_config("github: {push_on: [on_merge]}\n"));
  ASSERT_FALSE(push_event.has_value());
  EXPECT_NE(push_event.error().message.find("on_merge"), std::string::npos);

  auto push_empty =
      cf::cli::load_config(write_config("github: {push_on: []}\n"));
  ASSERT_FALSE(push_empty.has_value());
  EXPECT_NE(push_empty.error().message.find("github.push_on"),
            std::string::npos);

  auto index_version = cf::cli::load_config(write_config(
      "reference_docs: {manifest: m.yaml, index_version: latest}\n"));
  ASSERT_FALSE(index_version.has_value());
  EXPECT_NE(index_version.error().message.find("index_version"), std::string::npos);

  auto version_source = cf::cli::load_config(write_config(
      "reference_docs: {manifest: m.yaml, version_source: lockfile}\n"));
  ASSERT_FALSE(version_source.has_value());
  EXPECT_NE(version_source.error().message.find("version_source"),
            std::string::npos);

  auto coverage = cf::cli::load_config(
      write_config("gates: {coverage: {line: 1.5}}\n"));
  ASSERT_FALSE(coverage.has_value());
  EXPECT_NE(coverage.error().message.find("gates.coverage.line"),
            std::string::npos);

  auto mutation = cf::cli::load_config(
      write_config("gates: {mutation: {min_score: 0.6, enabled: yes}}\n"));
  ASSERT_TRUE(mutation.has_value()) << mutation.error().message;

  auto deadline = cf::cli::load_config(
      write_config("approvals: {design_approval: 5h}\n"));
  ASSERT_FALSE(deadline.has_value());
  EXPECT_NE(deadline.error().message.find("approvals.design_approval"),
            std::string::npos);

  auto network = cf::cli::load_config(
      write_config("sandbox: {network: allow}\n"));
  ASSERT_FALSE(network.has_value());
  EXPECT_NE(network.error().message.find("sandbox.network"), std::string::npos);

  auto severity = cf::cli::load_config(
      write_config("review: {blocking_severities: [blocking]}\n"));
  ASSERT_FALSE(severity.has_value());
  EXPECT_NE(severity.error().message.find("blocking"), std::string::npos);

  auto gate_policy = cf::cli::load_config(
      write_config("gates: {coverage: {line: 0.85, bogus: 1}}\n"));
  ASSERT_FALSE(gate_policy.has_value());
  EXPECT_NE(gate_policy.error().message.find("bogus"), std::string::npos);
}

TEST_F(ConfigTest, SecretManagerKeyReferencesAreRejected) {
  auto config = cf::cli::load_config(
      write_config("github: {private_key_ref: \"secretmanager://cf/key\"}\n"));
  ASSERT_FALSE(config.has_value());
  EXPECT_NE(config.error().message.find("secret manager"), std::string::npos);

  auto signing = cf::cli::load_config(
      write_config("github: {signing_key_ref: \"secretmanager://cf/sign\"}\n"));
  ASSERT_FALSE(signing.has_value());
  EXPECT_NE(signing.error().message.find("secret manager"), std::string::npos);
}

TEST_F(ConfigTest, UnknownRolesAndBadTypesAreRejected) {
  auto role = cf::cli::load_config(
      write_config("models: {roles: {tester: {fireworks: \"x\"}}}\n"));
  ASSERT_FALSE(role.has_value());
  EXPECT_NE(role.error().message.find("tester"), std::string::npos);

  auto type = cf::cli::load_config(
      write_config("budget: {max_usd_total: \"a lot\"}\n"));
  ASSERT_FALSE(type.has_value());
  EXPECT_NE(type.error().message.find("max_usd_total"), std::string::npos);
}

TEST_F(ConfigTest, RejectsInvalidRagSettings) {
  auto index = cf::cli::load_config(write_config("rag: {index: faiss}\n"));
  ASSERT_FALSE(index.has_value());
  EXPECT_NE(index.error().message.find("rag.index"), std::string::npos);

  auto top_k = cf::cli::load_config(write_config("rag: {top_k: 0}\n"));
  ASSERT_FALSE(top_k.has_value());
  EXPECT_NE(top_k.error().message.find("rag.top_k"), std::string::npos);

  auto weights = cf::cli::load_config(
      write_config("rag: {hybrid: {bm25_weight: 0, vector_weight: 0}}\n"));
  ASSERT_FALSE(weights.has_value());
  EXPECT_NE(weights.error().message.find("rag.hybrid"), std::string::npos);

  auto precedence = cf::cli::load_config(
      write_config("rag: {precedence: [policy, policy]}\n"));
  ASSERT_FALSE(precedence.has_value());
  EXPECT_NE(precedence.error().message.find("rag.precedence"),
            std::string::npos);
}

TEST_F(ConfigTest, ApplyConfigResolvesProvidersAndRoles) {
  auto config = cf::cli::load_config(write_config(kFull));
  ASSERT_TRUE(config.has_value()) << config.error().message;

  ExecuteOptions options;
  ASSERT_TRUE(cf::cli::apply_config(options, *config).has_value());

  EXPECT_EQ(options.workspace, "/srv/cf/work");
  EXPECT_EQ(options.repository_path, "/srv/checkouts/product-y");
  EXPECT_EQ(options.base_branch, "main");
  EXPECT_EQ(options.profiles_dir, "/srv/cf/profiles");
  EXPECT_EQ(options.github_app_id, "12345");
  EXPECT_EQ(options.github_private_key_file, "/srv/keys/github-app.pem");
  EXPECT_EQ(options.github_signing_key_file, "/srv/keys/cf-signing");
  EXPECT_EQ(*options.github_branch_prefix, "cf/");
  EXPECT_EQ(*options.github_protected_paths,
            (std::vector<std::string>{".github/**", "CODEOWNERS", "deploy/**"}));
  EXPECT_EQ(*options.github_max_file_mb, 7u);
  EXPECT_EQ(*options.github_push_on,
            (std::vector<std::string>{"run_done", "escalation"}));
  EXPECT_EQ(*options.branch_retention_days, 14u);
  EXPECT_FALSE(*options.github_pr_draft_until_done);
  EXPECT_EQ(*options.github_pr_labels,
            (std::vector<std::string>{"cf-generated", "automated"}));
  EXPECT_FALSE(*options.github_pr_request_codeowners);

  // The default provider resolves its endpoints; role models resolve
  // against it (design section 13.1: never mixed).
  EXPECT_EQ(options.provider_base_url, "https://api.fireworks.ai/inference/v1");
  EXPECT_EQ(options.api_key_env, "FIREWORKS_API_KEY");
  EXPECT_EQ(options.role_models.at("coder"),
            "accounts/fireworks/models/deepseek-v4p1-flash");
  EXPECT_EQ(options.role_models.at("reviewer"), "accounts/fireworks/models/kimi-k3");
  EXPECT_EQ(options.role_models.at("qa"), "accounts/fireworks/models/glm-5p3");
  EXPECT_EQ(options.design_model, "accounts/fireworks/models/deepseek-v4p1");
  EXPECT_EQ(options.embeddings_model, "accounts/fireworks/models/embed");
  EXPECT_EQ(*options.embeddings_dim, 768u);
  EXPECT_EQ(options.embeddings_base_url,
            "https://api.fireworks.ai/inference/v1");
  EXPECT_EQ(options.embeddings_api_key_env, "FIREWORKS_API_KEY");
  EXPECT_DOUBLE_EQ(options.role_options.at("coder")["temperature"].get<double>(),
                   0.1);
  EXPECT_EQ(options.role_options.at("coder")["max_output_tokens"].get<int>(),
            8192);
  // qa declares neither knob: no options entry is created for it.
  EXPECT_TRUE(options.role_options.find("qa") == options.role_options.end());

  EXPECT_DOUBLE_EQ(*options.budget_usd, 150.0);
  EXPECT_EQ(*options.max_iterations_per_milestone, 12u);
  EXPECT_DOUBLE_EQ(*options.budget_wall_clock_hours, 24.0);
  EXPECT_EQ(*options.design_approval_business_days, 5u);
  EXPECT_EQ(*options.escalation_business_days, 3u);
  EXPECT_EQ(*options.waiver_business_days, 2u);
  EXPECT_EQ(*options.reference_docs_manifest, "/srv/refs/manifest.yaml");
  EXPECT_EQ(*options.reference_docs_version_source, "auto");
  EXPECT_EQ(*options.reference_docs_on_missing_version, "report");
  EXPECT_EQ(*options.rag_index, "hnswlib");
  EXPECT_EQ(*options.rag_top_k, 12u);
  EXPECT_DOUBLE_EQ(*options.rag_bm25_weight, 0.4);
  EXPECT_DOUBLE_EQ(*options.rag_vector_weight, 0.6);
  EXPECT_EQ(*options.rag_precedence,
            (std::vector<std::string>{"design", "policy", "requirements",
                                      "architecture", "guideline", "pattern",
                                      "external_reference"}));
  EXPECT_EQ(*options.review_diff_threshold_lines, 250u);
  EXPECT_EQ(*options.review_blocking_severities,
            (std::vector<std::string>{"blocker", "major", "minor"}));
  EXPECT_DOUBLE_EQ(*options.sandbox_cpus, 8.0);
  EXPECT_DOUBLE_EQ(*options.sandbox_memory_gb, 16.0);
  EXPECT_EQ(*options.gate_timeout_min, 30u);
  EXPECT_EQ(*options.profile_id, "c-cpp");
  EXPECT_DOUBLE_EQ(*options.coverage_line, 0.9);
  EXPECT_DOUBLE_EQ(*options.coverage_branch, 0.8);
  EXPECT_TRUE(*options.mutation_enabled);
  EXPECT_DOUBLE_EQ(*options.mutation_min_score, 0.7);
}

TEST_F(ConfigTest, ApplyConfigRejectsAMissingRoleForTheProvider) {
  auto config = cf::cli::load_config(write_config(R"yaml(
models:
  default_provider: local-vllm
  roles:
    coder:    { local-vllm: "deepseek-local" }
    reviewer: { local-vllm: "kimi-local" }
    qa:       { fireworks: "glm-only-on-fireworks" }
)yaml"));
  ASSERT_TRUE(config.has_value()) << config.error().message;
  ExecuteOptions options;
  const auto applied = cf::cli::apply_config(options, *config);
  ASSERT_FALSE(applied.has_value());
  EXPECT_NE(applied.error().message.find("qa"), std::string::npos);
  EXPECT_NE(applied.error().message.find("local-vllm"), std::string::npos);
}

TEST_F(ConfigTest, ApplyConfigRequiresTheDefaultProviderToBeConfigured) {
  auto config = cf::cli::load_config(write_config(R"yaml(
providers:
  fireworks: { base_url: "https://api.fireworks.ai/inference/v1", api_key_env: "FIREWORKS_API_KEY" }
models:
  default_provider: local-vllm
)yaml"));
  ASSERT_TRUE(config.has_value()) << config.error().message;
  ExecuteOptions options;
  const auto applied = cf::cli::apply_config(options, *config);
  ASSERT_FALSE(applied.has_value());
  EXPECT_NE(applied.error().message.find("local-vllm"), std::string::npos);
}

}  // namespace
