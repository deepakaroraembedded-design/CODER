// cf-orch: opencode role configuration (design section 6.6).
//
// Builds each role's opencode.json from policy: the provider and model, the
// permission block (edit, bash allow/deny lists derived from the toolchain
// profile, everything else ask or deny), and the role's cf-mcp server with
// its run/milestone identity and calls file.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cf::orch {

struct RoleConfigSpec {
  std::string role;  // coder | reviewer | qa
  std::string provider_base_url = "https://api.fireworks.ai/inference/v1";
  std::string api_key_env = "FIREWORKS_API_KEY";
  std::string provider_model_id;  // e.g. accounts/fireworks/models/<model>
  // Extra provider/model options written verbatim into the model config
  // (opencode model `options`), e.g. {"reasoning_effort": "low"} so a
  // reasoning model does not spend its whole turn thinking. Empty omits the
  // block.
  nlohmann::json provider_options = nlohmann::json::object();
  std::vector<std::string> allowed_commands;  // profile agent_commands
  std::string mcp_command;                    // path to the cf-mcp binary
  std::string run_id;
  std::string milestone_id;
  std::string db_path;
  std::string docs_dir;
  // Offline reference documentation and its pinned version map (design
  // section 6.3.1); empty omits them.
  std::string reference_docs_dir;
  std::string reference_versions_file;
  // The run's cf-codeintel symbol index (design section 6.5); empty omits
  // the --codeintel-db flag.
  std::string codeintel_db;
  // RAG applicability context for the role's cf-mcp (design section 6.3):
  // the run's product and target language, so scoped documents apply.
  std::string product;
  std::string language;
  // Approved design packages (design section 6.2 step 7); empty omits them.
  std::string designs_dir;
  // Pattern catalog directory (design section 6.2); empty omits it.
  std::string patterns_dir;
  // Retrieval policy forwarded to cf-mcp (design sections 6.3, 15).
  std::optional<std::uint32_t> rag_top_k;
  std::optional<std::string> rag_index;
  std::optional<double> rag_bm25_weight;
  std::optional<double> rag_vector_weight;
  std::optional<std::vector<std::string>> rag_precedence;
  // Embeddings for the role's hybrid retriever (design section 6.3).
  std::string embeddings_model;
  std::string embeddings_base_url;
  std::string embeddings_api_key_env;
  std::uint32_t waiver_deadline_business_days = 0;  // 0: omit the flag
  std::string calls_file;  // CF_MCP_CALLS_FILE for this role
  // Agent-side gate dry run (run_checks, design section 9.1). Empty
  // profile_path leaves the tool unavailable.
  std::string profile_path;
  std::string rules_dir;
  std::string check_build_dir;
  std::string profile_build_system;
  std::string profile_test_framework;
  std::string target_standard;
  std::string standard_var;
};

nlohmann::json build_role_config(const RoleConfigSpec& spec);

}  // namespace cf::orch
