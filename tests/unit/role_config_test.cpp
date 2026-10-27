// Unit tests for the opencode role configuration (design section 6.6).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cf/orch/role_config.hpp"

namespace {

using cf::orch::build_role_config;
using cf::orch::RoleConfigSpec;
using Json = nlohmann::json;

RoleConfigSpec base_spec(const std::string& role) {
  RoleConfigSpec spec;
  spec.role = role;
  spec.provider_model_id = "accounts/fireworks/models/deepseek-v4p1-flash";
  spec.allowed_commands = {"cmake", "ninja", "ctest"};
  spec.mcp_command = "/opt/cf/cf-mcp";
  spec.run_id = "R12";
  spec.milestone_id = "M3";
  spec.db_path = "/work/runs/R12/run.db";
  spec.docs_dir = "/work/docs";
  spec.product = "Y";
  spec.language = "cpp";
  spec.designs_dir = "/work/designs";
  spec.patterns_dir = "/work/catalog/patterns";
  spec.calls_file = "/work/runs/R12/calls-coder.jsonl";
  return spec;
}

TEST(RoleConfigTest, CoderConfigAllowsEditsAndProfileCommands) {
  const Json config = build_role_config(base_spec("coder"));
  EXPECT_EQ(config["$schema"], "https://opencode.ai/config.json");
  EXPECT_EQ(config["model"], "fireworks/coder-a");
  // Title/summary requests must not inherit the user's global small_model.
  EXPECT_EQ(config["small_model"], "fireworks/coder-a");
  EXPECT_EQ(config["provider"]["fireworks"]["options"]["baseURL"],
            "https://api.fireworks.ai/inference/v1");
  EXPECT_EQ(config["provider"]["fireworks"]["options"]["apiKey"],
            "{env:FIREWORKS_API_KEY}");
  EXPECT_EQ(config["provider"]["fireworks"]["models"]["coder-a"]["id"],
            "accounts/fireworks/models/deepseek-v4p1-flash");

  EXPECT_EQ(config["permission"]["edit"], "allow");
  EXPECT_EQ(config["permission"]["webfetch"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["cmake *"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["ninja"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["git status*"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["git push*"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["git commit*"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["curl *"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["*"], "ask");

  const Json mcp = config["mcp"]["cf"];
  EXPECT_EQ(mcp["type"], "local");
  EXPECT_EQ(mcp["command"][0], "/opt/cf/cf-mcp");
  EXPECT_EQ(mcp["command"][1], "--role=coder");
  EXPECT_EQ(mcp["command"][2], "--run=R12");
  EXPECT_EQ(mcp["command"][3], "--milestone=M3");
  EXPECT_EQ(mcp["command"][4], "--db=/work/runs/R12/run.db");
  EXPECT_EQ(mcp["command"][5], "--docs=/work/docs");
  // The RAG applicability context, approved design packages and the pattern
  // catalog are part of the MCP command (design sections 6.2 step 7, 6.3).
  EXPECT_EQ(mcp["command"][6], "--product=Y");
  EXPECT_EQ(mcp["command"][7], "--language=cpp");
  EXPECT_EQ(mcp["command"][8], "--designs=/work/designs");
  EXPECT_EQ(mcp["command"][9], "--patterns=/work/catalog/patterns");
  EXPECT_EQ(mcp["environment"]["CF_MCP_CALLS_FILE"],
            "/work/runs/R12/calls-coder.jsonl");
}

TEST(RoleConfigTest, CodeintelDbAddsTheFlagOnlyWhenSet) {
  RoleConfigSpec spec = base_spec("coder");
  spec.codeintel_db = "/work/runs/R12/codeintel.db";
  const Json config = build_role_config(spec);
  // Appended after --patterns (design sections 6.5, 9.1).
  EXPECT_EQ(config["mcp"]["cf"]["command"][10],
            "--codeintel-db=/work/runs/R12/codeintel.db");

  const Json without = build_role_config(base_spec("coder"));
  EXPECT_EQ(without["mcp"]["cf"]["command"].size(), 10u);
}

TEST(RoleConfigTest, RagSettingsAreForwardedToMcp) {
  RoleConfigSpec spec = base_spec("coder");
  spec.rag_top_k = 12;
  spec.rag_index = "brute-force";
  spec.rag_bm25_weight = 0.4;
  spec.rag_vector_weight = 0.6;
  spec.rag_precedence = {"design", "policy", "requirements"};
  const Json config = build_role_config(spec);
  const Json command = config["mcp"]["cf"]["command"];
  EXPECT_EQ(command[10], "--top-k=12");
  EXPECT_EQ(command[11], "--rag-index=brute-force");
  EXPECT_NE(command[12].get<std::string>().find("--rag-bm25-weight=0.4"),
            std::string::npos);
  EXPECT_NE(command[13].get<std::string>().find("--rag-vector-weight=0.6"),
            std::string::npos);
  EXPECT_EQ(command[14], "--rag-precedence=design,policy,requirements");
  EXPECT_EQ(command.size(), 15u);
}

TEST(RoleConfigTest, ReviewerIsReadOnlyAndDefaultsToDeny) {
  RoleConfigSpec spec = base_spec("reviewer");
  spec.allowed_commands.clear();
  const Json config = build_role_config(spec);
  EXPECT_EQ(config["model"], "fireworks/reviewer-b");
  EXPECT_EQ(config["permission"]["edit"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["*"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["git diff*"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["grep *"], "allow");
  EXPECT_FALSE(config["permission"]["bash"].contains("cmake *"));
}

TEST(RoleConfigTest, QaConfigAllowsEditsAndAsksForTheRest) {
  RoleConfigSpec spec = base_spec("qa");
  spec.allowed_commands = {"ctest", "pytest"};
  const Json config = build_role_config(spec);
  EXPECT_EQ(config["model"], "fireworks/qa-c");
  EXPECT_EQ(config["permission"]["edit"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["ctest *"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["pytest *"], "allow");
  EXPECT_EQ(config["permission"]["bash"]["git push*"], "deny");
  EXPECT_EQ(config["permission"]["bash"]["*"], "ask");
}

TEST(RoleConfigTest, ProviderOptionsAreWrittenVerbatim) {
  RoleConfigSpec spec = base_spec("qa");
  spec.provider_options = Json{{"reasoningEffort", "low"}};
  const Json config = build_role_config(spec);
  EXPECT_EQ(config["provider"]["fireworks"]["models"]["qa-c"]["options"]
                  ["reasoningEffort"],
            "low");

  // Without options the block is omitted entirely.
  const Json plain = build_role_config(base_spec("qa"));
  EXPECT_FALSE(plain["provider"]["fireworks"]["models"]["qa-c"].contains("options"));
}

TEST(RoleConfigTest, OptionalSectionsAreOmitted) {
  RoleConfigSpec spec = base_spec("coder");
  spec.milestone_id.clear();
  spec.db_path.clear();
  spec.docs_dir.clear();
  spec.product.clear();
  spec.language.clear();
  spec.designs_dir.clear();
  spec.patterns_dir.clear();
  spec.calls_file.clear();
  const Json config = build_role_config(spec);
  ASSERT_EQ(config["mcp"]["cf"]["command"].size(), 3u);
  EXPECT_FALSE(config["mcp"]["cf"].contains("environment"));
}

}  // namespace
