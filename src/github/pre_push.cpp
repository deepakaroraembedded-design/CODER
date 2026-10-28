#include "cf/github/pre_push.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include "cf/github/secrets.hpp"

namespace cf::github {
namespace {

constexpr uint64_t kScanLimitBytes = 1u << 20;  // scan files up to 1 MiB

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

// The commits a push would add: everything since the base ref, or just the
// pushed commit when the base is unknown. The pushed commit is always
// included (a fast-forward with no new base range still has to be signed).
Result<std::vector<std::string>> commits_to_verify(
    git::Repository& repository, const std::string& commit,
    const std::string& base_branch) {
  std::string range = commit;
  if (!base_branch.empty()) {
    for (const std::string& candidate :
         {"refs/remotes/origin/" + base_branch,
          "refs/heads/" + base_branch, base_branch}) {
      auto exists = repository.ref_exists(candidate);
      if (!exists) {
        return tl::make_unexpected(exists.error());
      }
      if (*exists) {
        range = candidate + ".." + commit;
        break;
      }
    }
  }
  auto listed = repository.command({"rev-list", range});
  if (!listed) {
    return tl::make_unexpected(listed.error());
  }
  std::vector<std::string> commits = split_lines(*listed);
  if (std::find(commits.begin(), commits.end(), commit) == commits.end()) {
    commits.push_back(commit);
  }
  return commits;
}

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

  // Design section 10.4.6 check 7: every commit this push adds must be
  // signed. Checking the raw object avoids needing an allowed-signers file
  // for SSH verification; GitHub verifies the signature against the key.
  if (config.require_signatures) {
    auto commits = commits_to_verify(repository, commit, config.base_branch);
    if (!commits) {
      return tl::make_unexpected(commits.error());
    }
    for (const auto& sha : *commits) {
      auto object = repository.command({"cat-file", "-p", sha});
      if (!object) {
        return tl::make_unexpected(object.error());
      }
      // Only the header block counts: a commit message that happens to
      // mention "gpgsig" must not make an unsigned commit look signed.
      const std::size_t headers_end = object->find("\n\n");
      const std::string_view headers =
          headers_end == std::string::npos
              ? std::string_view(*object)
              : std::string_view(*object).substr(0, headers_end);
      if (headers.find("\ngpgsig ") == std::string_view::npos) {
        findings.push_back(
            {"signature", "commit " + sha.substr(0, 12) + " is not signed"});
      }
    }
  }
  return findings;
}

}  // namespace cf::github
