#include "cf/agents/permission_broker.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <set>
#include <utility>

namespace cf::agents {
namespace {

struct CommandAnalysis {
  bool substitution = false;
  bool chaining = false;
  bool redirection = false;
};

// Scans a command line, respecting quotes: `;`, `&`, `|`, subshells and
// redirection outside quotes (and substitution anywhere except single
// quotes) mark the command as non-simple (design section 6.6.1 rule 3).
CommandAnalysis analyze_command(std::string_view command) {
  CommandAnalysis analysis;
  char quote = 0;
  for (std::size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      }
      continue;
    }
    if (quote == '"') {
      if (c == '\\') {
        ++i;
        continue;
      }
      if (c == '"') {
        quote = 0;
        continue;
      }
      if (c == '`' || (c == '$' && i + 1 < command.size() && command[i + 1] == '(')) {
        analysis.substitution = true;
      }
      continue;
    }
    if (c == '\\') {
      ++i;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '`' || (c == '$' && i + 1 < command.size() && command[i + 1] == '(')) {
      analysis.substitution = true;
      continue;
    }
    if (c == ';' || c == '&' || c == '|') {
      analysis.chaining = true;
      continue;
    }
    if (c == '<' || c == '>') {
      analysis.redirection = true;
      continue;
    }
    if (c == '(' || c == ')') {
      analysis.chaining = true;
      continue;
    }
  }
  return analysis;
}

// Splits a simple command into tokens, respecting quotes.
std::vector<std::string> split_command(std::string_view command) {
  std::vector<std::string> tokens;
  std::string current;
  char quote = 0;
  const auto flush = [&tokens, &current]() {
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  };
  for (std::size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (quote == '\'') {
      if (c == '\'') {
        quote = 0;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (quote == '"') {
      if (c == '\\' && i + 1 < command.size()) {
        current.push_back(command[++i]);
      } else if (c == '"') {
        quote = 0;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '\\' && i + 1 < command.size()) {
      current.push_back(command[++i]);
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      flush();
      continue;
    }
    current.push_back(c);
  }
  flush();
  return tokens;
}

std::string trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string base_name(std::string_view program) {
  const std::size_t slash = program.rfind('/');
  return slash == std::string_view::npos ? std::string(program)
                                         : std::string(program.substr(slash + 1));
}

bool has_flag(const std::vector<std::string>& tokens, std::string_view flag) {
  return std::any_of(tokens.begin(), tokens.end(), [flag](const std::string& token) {
    return token == flag;
  });
}

// Wrappers that run another command (design section 6.6.1 rule 3).
bool is_wrapper(const std::vector<std::string>& tokens) {
  if (tokens.empty()) {
    return false;
  }
  const std::string program = base_name(tokens.front());
  static const std::set<std::string> kShells = {"bash", "sh", "dash", "zsh", "ksh"};
  if (kShells.count(program) > 0) {
    return true;
  }
  static const std::set<std::string> kAlways = {"env", "xargs", "nohup", "timeout"};
  if (kAlways.count(program) > 0) {
    return true;
  }
  static const std::set<std::string> kInterpreters = {"python", "python3", "node",
                                                      "nodejs", "perl", "ruby"};
  if (kInterpreters.count(program) > 0 && (has_flag(tokens, "-c") ||
                                           has_flag(tokens, "-e") ||
                                           has_flag(tokens, "--eval"))) {
    return true;
  }
  if (program == "find" && has_flag(tokens, "-exec")) {
    return true;
  }
  return false;
}

bool matches_entry(const std::string& command, const std::string& entry) {
  if (command == entry) {
    return true;
  }
  return command.size() > entry.size() && command.compare(0, entry.size(), entry) == 0 &&
         command[entry.size()] == ' ';
}

bool inside(const std::filesystem::path& root, const std::filesystem::path& path) {
  const std::string root_text = root.string();
  const std::string path_text = path.string();
  if (path_text == root_text) {
    return true;
  }
  return path_text.size() > root_text.size() &&
         path_text.compare(0, root_text.size(), root_text) == 0 &&
         path_text[root_text.size()] == '/';
}

PermissionDecision reject(std::string rule_id, std::string reason) {
  PermissionDecision decision;
  decision.reply = PermissionReply::Reject;
  decision.rule_id = std::move(rule_id);
  decision.reason = std::move(reason);
  return decision;
}

PermissionDecision allow_once(std::string rule_id) {
  PermissionDecision decision;
  decision.reply = PermissionReply::Once;
  decision.rule_id = std::move(rule_id);
  return decision;
}

}  // namespace

PermissionPolicy::PermissionPolicy(PermissionPolicyConfig config)
    : config_(std::move(config)) {}

PermissionDecision PermissionPolicy::decide_bash(
    const PermissionRequest& request) const {
  std::string command = trim(request.command_or_path);
  if (command.empty() && !request.patterns.empty()) {
    command = trim(request.patterns.front());
  }
  if (command.empty()) {
    return reject("default-deny", "empty command");
  }

  // Rule 1: the deny list wins over everything else.
  for (const auto& entry : config_.deny_commands) {
    if (matches_entry(command, entry)) {
      return reject("deny-list", "command is denied by policy: " + entry);
    }
  }

  const std::vector<std::string> tokens = split_command(command);
  if (tokens.empty()) {
    return reject("default-deny", "empty command");
  }

  // An explicitly listed full command is allowed as-is (this is the only way
  // a wrapper such as `bash -c ./configure` can run).
  for (const auto& entry : config_.allowed_commands) {
    if (entry.find(' ') != std::string::npos && entry == command) {
      return allow_once("profile-command");
    }
  }

  const CommandAnalysis analysis = analyze_command(command);
  if (analysis.substitution) {
    return reject("command-substitution",
                  "command substitution is not permitted");
  }
  if (analysis.chaining) {
    return reject("command-chaining",
                  "chaining, pipes and subshells are not permitted");
  }
  if (analysis.redirection) {
    return reject("redirection", "redirection is not permitted");
  }
  if (is_wrapper(tokens)) {
    return reject("command-wrapper",
                  "wrappers that run another command are not permitted");
  }

  const std::string program = base_name(tokens.front());
  for (const auto& entry : config_.allowed_commands) {
    if (entry == program) {
      return allow_once("profile-command");
    }
  }
  return reject("default-deny",
                "command is not in the toolchain profile: " + program);
}

PermissionDecision PermissionPolicy::decide_path(const PermissionRequest& request,
                                                 bool writable) const {
  std::filesystem::path path = trim(request.command_or_path);
  if (path.is_relative()) {
    path = std::filesystem::path(config_.worktree) / path;
  }
  std::error_code ec;
  // weakly_canonical resolves `..` and existing symlinks, so neither can
  // escape the worktree (design section 6.6.1 rule 2).
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    return reject("path-unresolvable", "cannot resolve path: " + request.command_or_path);
  }
  if (!inside(std::filesystem::path(config_.worktree), resolved)) {
    return reject("path-outside-worktree",
                  "path is outside the agent's worktree: " + resolved.string());
  }
  if (!writable) {
    return allow_once("worktree-read");
  }
  if (config_.writable_paths.empty()) {
    return reject("read-only-role", "this role may not modify files");
  }
  for (const auto& relative : config_.writable_paths) {
    const std::filesystem::path root =
        std::filesystem::path(config_.worktree) / relative;
    if (inside(root, resolved)) {
      return allow_once("writable-path");
    }
  }
  return reject("path-outside-writable",
                "path is not in the role's writable mounts: " + resolved.string());
}

PermissionDecision PermissionPolicy::decide(const PermissionRequest& request) const {
  const std::string type = request.type;
  if (type == "bash") {
    return decide_bash(request);
  }
  if (type == "read" || type == "glob" || type == "grep" || type == "list") {
    return decide_path(request, false);
  }
  if (type == "edit" || type == "write") {
    return decide_path(request, true);
  }
  if (type == "todowrite" || type == "question") {
    return allow_once("harmless-tool");
  }
  if (type == "webfetch" || type == "websearch" || type == "task" ||
      type == "external_directory" || type == "lsp" || type == "doom_loop" ||
      type == "skill") {
    return reject("tool-not-allowed", "the " + type + " tool is not permitted");
  }
  return reject("unknown-permission-type",
                "unrecognized permission type '" + type +
                    "'; flagged for a policy update");
}

PermissionBroker::PermissionBroker(PermissionPolicy policy, ReplyFn reply)
    : policy_(std::move(policy)), reply_(std::move(reply)) {}

PermissionDecision PermissionBroker::handle(const PermissionRequest& request) {
  const PermissionDecision decision = policy_.decide(request);
  if (decision.reply == PermissionReply::Reject) {
    ++rejected_in_turn_;
  }
  if (reply_) {
    reply_(request, decision);
  }
  return decision;
}

}  // namespace cf::agents
