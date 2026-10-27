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
  branch_prefix: "cf/"
  protected_paths: [".github/**", "CODEOWNERS", "deploy/**"]
  max_file_mb: 7
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
budget:
  max_usd_total: 150
  max_tokens_total: 60000000
  max_iterations_per_milestone: 12
  max_attempts_per_defect: 4
  max_wall_clock_hours: 24
approvals:
  design_approval: 5d
  escalation: 3d
gates:
  profiles_dir: /srv/cf/profiles
review:
  diff_threshold_lines: 250
sandbox:
  cpu: 8
  memory_gb: 16
  job_timeout_min: 30
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
  EXPECT_EQ(*config->github.branch_prefix, "cf/");
  EXPECT_EQ(*config->github.protected_paths,
            (std::vector<std::string>{".github/**", "CODEOWNERS", "deploy/**"}));
  EXPECT_EQ(*config->github.max_file_mb, 7u);
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

  EXPECT_DOUBLE_EQ(*config->budget.max_usd_total, 150.0);
  EXPECT_EQ(*config->budget.max_tokens_total, 60000000u);
  EXPECT_EQ(*config->budget.max_iterations_per_milestone, 12u);
  EXPECT_EQ(*config->budget.max_attempts_per_defect, 4u);
  EXPECT_DOUBLE_EQ(*config->budget.max_wall_clock_hours, 24.0);
  EXPECT_EQ(*config->approvals.design_approval_business_days, 5u);
  EXPECT_EQ(*config->approvals.escalation_business_days, 3u);
  EXPECT_EQ(*config->gates.profiles_dir, "/srv/cf/profiles");
  EXPECT_EQ(*config->review.diff_threshold_lines, 250u);
  EXPECT_DOUBLE_EQ(*config->sandbox.cpu, 8.0);
  EXPECT_DOUBLE_EQ(*config->sandbox.memory_gb, 16.0);
  EXPECT_EQ(*config->sandbox.job_timeout_min, 30u);
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
  auto unsupported = cf::cli::load_config(write_config("approvals: {waiver: 2d}\n"));
  ASSERT_FALSE(unsupported.has_value());
  EXPECT_NE(unsupported.error().message.find("not supported"),
            std::string::npos);

  auto deadline = cf::cli::load_config(
      write_config("approvals: {design_approval: 5h}\n"));
  ASSERT_FALSE(deadline.has_value());
  EXPECT_NE(deadline.error().message.find("approvals.design_approval"),
            std::string::npos);

  auto gate_policy = cf::cli::load_config(write_config("gates: {coverage: {line: 0.85}}\n"));
  ASSERT_FALSE(gate_policy.has_value());
  EXPECT_NE(gate_policy.error().message.find("gates.coverage"), std::string::npos);
}

TEST_F(ConfigTest, SecretManagerKeyReferencesAreRejected) {
  auto config = cf::cli::load_config(
      write_config("github: {private_key_ref: \"secretmanager://cf/key\"}\n"));
  ASSERT_FALSE(config.has_value());
  EXPECT_NE(config.error().message.find("secret manager"), std::string::npos);
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
  EXPECT_EQ(*options.github_branch_prefix, "cf/");
  EXPECT_EQ(*options.github_protected_paths,
            (std::vector<std::string>{".github/**", "CODEOWNERS", "deploy/**"}));
  EXPECT_EQ(*options.github_max_file_mb, 7u);
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
  EXPECT_EQ(*options.review_diff_threshold_lines, 250u);
  EXPECT_DOUBLE_EQ(*options.sandbox_cpus, 8.0);
  EXPECT_DOUBLE_EQ(*options.sandbox_memory_gb, 16.0);
  EXPECT_EQ(*options.gate_timeout_min, 30u);
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
