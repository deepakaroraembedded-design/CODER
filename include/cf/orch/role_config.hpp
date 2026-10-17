// cf-orch: opencode role configuration (design section 6.6).
//
// Builds each role's opencode.json from policy: the provider and model, the
// permission block (edit, bash allow/deny lists derived from the toolchain
// profile, everything else ask or deny), and the role's cf-mcp server with
// its run/milestone identity and calls file.
#pragma once

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
  // RAG applicability context for the role's cf-mcp (design section 6.3):
  // the run's product and target language, so scoped documents apply.
  std::string product;
  std::string language;
  // Approved design packages (design section 6.2 step 7); empty omits them.
  std::string designs_dir;
  // Pattern catalog directory (design section 6.2); empty omits it.
  std::string patterns_dir;
  std::string calls_file;  // CF_MCP_CALLS_FILE for this role
};

nlohmann::json build_role_config(const RoleConfigSpec& spec);

}  // namespace cf::orch
