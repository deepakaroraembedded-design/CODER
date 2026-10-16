#include "cf/github/pre_push.hpp"

#include <set>
#include <utility>

#include "cf/github/secrets.hpp"

namespace cf::github {
namespace {

constexpr uint64_t kScanLimitBytes = 1u << 20;  // scan files up to 1 MiB

bool matches_pattern(const std::string& path, const std::string& pattern) {
  if (pattern.size() > 2 && pattern.compare(pattern.size() - 2, 2, "**") == 0) {
    std::string prefix = pattern.substr(0, pattern.size() - 2);
    return path.compare(0, prefix.size(), prefix) == 0;
  }
  return path == pattern;
}

}  // namespace

Result<std::vector<PrePushFinding>> check_pre_push(git::Repository& repository,
                                                   const std::string& branch,
                                                   const std::string& commit,
                                                   const PrePushConfig& config) {
  std::vector<PrePushFinding> findings;

  if (branch.rfind(config.branch_prefix, 0) != 0) {
    findings.push_back({"branch-policy",
                        "branch '" + branch + "' must start with '" +
                            config.branch_prefix + "'"});
  }

  // Changes introduced by the commit (against its first parent).
  auto parent = repository.head_sha(repository.path(), commit + "^");
  std::vector<git::FileChange> changes;
  if (parent.has_value()) {
    auto diff = repository.diff(*parent, commit);
    if (!diff) {
      return tl::make_unexpected(diff.error());
    }
    changes = std::move(*diff);
  }

  for (const auto& change : changes) {
    for (const auto& pattern : config.protected_paths) {
      if (matches_pattern(change.path, pattern)) {
        findings.push_back({"protected-path",
                            "'" + change.path + "' is protected (pattern '" +
                                pattern + "') and needs a human"});
      }
    }
    if (change.binary && !config.allow_binaries) {
      findings.push_back({"binary-file",
                          "'" + change.path + "' is a binary file"});
    }
    if (change.status != 'D') {
      auto size = repository.file_size(commit, change.path);
      if (size.has_value()) {
        const uint64_t limit = config.max_file_mb * 1024 * 1024;
        if (*size > limit) {
          findings.push_back({"file-size",
                              "'" + change.path + "' is " + std::to_string(*size) +
                                  " bytes (limit " + std::to_string(limit) + ")"});
        }
        if (!change.binary && *size <= kScanLimitBytes) {
          auto content = repository.file_content(commit, change.path);
          if (content.has_value()) {
            for (const auto& secret : scan_for_secrets(*content)) {
              findings.push_back({"secret",
                                  "'" + change.path + "' line " +
                                      std::to_string(secret.line) + " matches " +
                                      secret.pattern});
            }
          }
        }
      }

      // New dependencies in changed manifests (design section 10.4.6 #6).
      if (config.licenses != nullptr) {
        auto new_content = repository.file_content(commit, change.path);
        if (new_content.has_value()) {
          auto new_dependencies = dependencies_in_manifest(change.path, *new_content);
          if (new_dependencies.has_value() && !new_dependencies->empty()) {
            std::set<std::string> previous;
            if (parent.has_value()) {
              auto old_content = repository.file_content(*parent, change.path);
              if (old_content.has_value()) {
                auto old_dependencies =
                    dependencies_in_manifest(change.path, *old_content);
                if (old_dependencies.has_value()) {
                  previous.insert(old_dependencies->begin(), old_dependencies->end());
                }
              }
            }
            std::vector<std::string> added;
            for (const auto& dependency : *new_dependencies) {
              if (previous.count(dependency) == 0) {
                added.push_back(dependency);
              }
            }
            for (const auto& finding :
                 check_licenses(added, *config.licenses, config.license_policy)) {
              findings.push_back({"license", finding.reason});
            }
          }
        }
      }
    }
  }

  auto message = repository.commit_message(commit);
  if (!message) {
    return tl::make_unexpected(message.error());
  }
  for (const auto& trailer : config.required_trailers) {
    if (message->find(trailer + ":") == std::string::npos) {
      findings.push_back({"trailer", "commit is missing the " + trailer + " trailer"});
    }
  }
  return findings;
}

}  // namespace cf::github
