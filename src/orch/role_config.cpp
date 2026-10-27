#include "cf/orch/role_config.hpp"

namespace cf::orch {
namespace {

using Json = nlohmann::json;

std::string model_alias(const std::string& role) {
  if (role == "reviewer") {
    return "reviewer-b";
  }
  if (role == "qa") {
    return "qa-c";
  }
  return "coder-a";
}

}  // namespace

nlohmann::json build_role_config(const RoleConfigSpec& spec) {
  const std::string alias = model_alias(spec.role);

  Json config;
  config["$schema"] = "https://opencode.ai/config.json";
  config["provider"]["fireworks"]["npm"] = "@ai-sdk/openai-compatible";
  config["provider"]["fireworks"]["options"]["baseURL"] = spec.provider_base_url;
  config["provider"]["fireworks"]["options"]["apiKey"] =
      "{env:" + spec.api_key_env + "}";
  config["provider"]["fireworks"]["models"][alias]["id"] = spec.provider_model_id;
  if (!spec.provider_options.empty()) {
    config["provider"]["fireworks"]["models"][alias]["options"] = spec.provider_options;
  }
  config["model"] = "fireworks/" + alias;
  // Title and summary requests must not inherit a `small_model` from the
  // user's global opencode config (it may point at a provider this machine
  // cannot reach, which stalls every session behind a failing request).
  config["small_model"] = "fireworks/" + alias;

  // Permission block (design section 6.6.1 layer 1): almost everything is
  // decided in advance, so the broker only sees the remainder.
  Json permission;
  permission["webfetch"] = "deny";
  permission["edit"] = spec.role == "reviewer" ? "deny" : "allow";

  Json bash = Json::object();
  bash["*"] = spec.role == "reviewer" ? "deny" : "ask";
  for (const auto& command : spec.allowed_commands) {
    bash[command] = "allow";
    bash[command + " *"] = "allow";
  }
  if (spec.role == "coder" || spec.role == "reviewer") {
    bash["git status*"] = "allow";
    bash["git diff*"] = "allow";
    bash["git log*"] = "allow";
  }
  if (spec.role == "reviewer") {
    bash["grep *"] = "allow";
    bash["cat *"] = "allow";
    bash["head *"] = "allow";
  }
  for (const char* deny :
       {"git push*", "git remote*", "git config*", "git reset*", "git add*",
        "git commit*", "curl *", "wget *", "ssh *", "sudo *"}) {
    bash[deny] = "deny";
  }
  permission["bash"] = bash;
  config["permission"] = permission;

  Json mcp_command = Json::array(
      {spec.mcp_command, "--role=" + spec.role, "--run=" + spec.run_id});
  if (!spec.milestone_id.empty()) {
    mcp_command.push_back("--milestone=" + spec.milestone_id);
  }
  if (!spec.db_path.empty()) {
    mcp_command.push_back("--db=" + spec.db_path);
  }
  if (!spec.docs_dir.empty()) {
    mcp_command.push_back("--docs=" + spec.docs_dir);
  }
  if (!spec.product.empty()) {
    mcp_command.push_back("--product=" + spec.product);
  }
  if (!spec.language.empty()) {
    mcp_command.push_back("--language=" + spec.language);
  }
  if (!spec.designs_dir.empty()) {
    mcp_command.push_back("--designs=" + spec.designs_dir);
  }
  if (!spec.patterns_dir.empty()) {
    mcp_command.push_back("--patterns=" + spec.patterns_dir);
  }
  if (!spec.codeintel_db.empty()) {
    mcp_command.push_back("--codeintel-db=" + spec.codeintel_db);
  }
  if (spec.rag_top_k.has_value()) {
    mcp_command.push_back("--top-k=" + std::to_string(*spec.rag_top_k));
  }
  if (spec.rag_index.has_value()) {
    mcp_command.push_back("--rag-index=" + *spec.rag_index);
  }
  if (spec.rag_bm25_weight.has_value()) {
    mcp_command.push_back("--rag-bm25-weight=" +
                          std::to_string(*spec.rag_bm25_weight));
  }
  if (spec.rag_vector_weight.has_value()) {
    mcp_command.push_back("--rag-vector-weight=" +
                          std::to_string(*spec.rag_vector_weight));
  }
  if (spec.rag_precedence.has_value() && !spec.rag_precedence->empty()) {
    std::string joined;
    for (const auto& type : *spec.rag_precedence) {
      if (!joined.empty()) {
        joined += ",";
      }
      joined += type;
    }
    mcp_command.push_back("--rag-precedence=" + joined);
  }
  config["mcp"]["cf"] = Json{{"type", "local"},
                             {"command", std::move(mcp_command)},
                             {"enabled", true}};
  if (!spec.calls_file.empty()) {
    config["mcp"]["cf"]["environment"] =
        Json{{"CF_MCP_CALLS_FILE", spec.calls_file}};
  }
  return config;
}

}  // namespace cf::orch
