// Unit tests for the Permission Broker (design section 6.6.1 layer 2).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/agents/permission_broker.hpp"

namespace {

using cf::agents::PermissionBroker;
using cf::agents::PermissionDecision;
using cf::agents::PermissionPolicy;
using cf::agents::PermissionPolicyConfig;
using cf::agents::PermissionReply;
using cf::agents::PermissionRequest;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_permission_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class PermissionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    worktree_ = make_temp_dir();
    std::filesystem::create_directories(worktree_ / "src");
    config_.worktree = worktree_.string();
    config_.writable_paths = {"src", "include"};
    config_.allowed_commands = {"cmake", "ninja", "ctest", "make"};
    config_.deny_commands = {"curl", "wget", "ssh", "sudo", "git push", "git remote"};
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(worktree_, ec);
  }

  PermissionRequest bash_request(const std::string& command) const {
    PermissionRequest request;
    request.type = "bash";
    request.command_or_path = command;
    return request;
  }

  PermissionRequest path_request(const std::string& type,
                                 const std::string& path) const {
    PermissionRequest request;
    request.type = type;
    request.command_or_path = path;
    return request;
  }

  std::filesystem::path worktree_;
  PermissionPolicyConfig config_;
};

TEST_F(PermissionTest, DenyListWinsOverEverything) {
  PermissionPolicy policy(config_);
  const auto decision = policy.decide(bash_request("git push origin main"));
  EXPECT_EQ(decision.reply, PermissionReply::Reject);
  EXPECT_EQ(decision.rule_id, "deny-list");
  EXPECT_EQ(policy.decide(bash_request("curl https://example.invalid")).rule_id,
            "deny-list");
  EXPECT_EQ(policy.decide(bash_request("sudo cmake --version")).rule_id, "deny-list");
}

TEST_F(PermissionTest, ProfileCommandsAreAllowedOnce) {
  PermissionPolicy policy(config_);
  const auto decision = policy.decide(bash_request("cmake --build /build -j8"));
  EXPECT_EQ(decision.reply, PermissionReply::Once);
  EXPECT_EQ(decision.rule_id, "profile-command");
  EXPECT_EQ(policy.decide(bash_request("ctest --test-dir /build")).reply,
            PermissionReply::Once);
}

TEST_F(PermissionTest, ChainingPipesAndSubshellsAreRejected) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(bash_request("cmake --build /build && rm -rf /")).rule_id,
            "command-chaining");
  EXPECT_EQ(policy.decide(bash_request("cat secrets | sh")).rule_id,
            "command-chaining");
  EXPECT_EQ(policy.decide(bash_request("cmake --build /build &")).rule_id,
            "command-chaining");
  EXPECT_EQ(policy.decide(bash_request("(cd /tmp && cmake)")).rule_id,
            "command-chaining");
}

TEST_F(PermissionTest, CommandSubstitutionIsRejected) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(bash_request("cmake --build $(pwd)")).rule_id,
            "command-substitution");
  EXPECT_EQ(policy.decide(bash_request("cmake --build `pwd`")).rule_id,
            "command-substitution");
  EXPECT_EQ(policy.decide(bash_request("echo \"$(cat /etc/passwd)\"")).rule_id,
            "command-substitution");
}

TEST_F(PermissionTest, RedirectionIsRejected) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(bash_request("cmake --build /build > /etc/passwd")).rule_id,
            "redirection");
  EXPECT_EQ(policy.decide(bash_request("ctest < /dev/urandom")).rule_id, "redirection");
}

TEST_F(PermissionTest, WrappersAreRejected) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(bash_request("bash -c 'rm -rf /'")).rule_id,
            "command-wrapper");
  EXPECT_EQ(policy.decide(bash_request("sh -c 'cmake --version'")).rule_id,
            "command-wrapper");
  EXPECT_EQ(policy.decide(bash_request("env cmake --version")).rule_id,
            "command-wrapper");
  EXPECT_EQ(policy.decide(bash_request("timeout 5 cmake --version")).rule_id,
            "command-wrapper");
  EXPECT_EQ(policy.decide(bash_request("python3 -c 'import os'")).rule_id,
            "command-wrapper");
  EXPECT_EQ(policy.decide(bash_request("find . -exec rm {} \\;")).rule_id,
            "command-wrapper");
}

TEST_F(PermissionTest, ExplicitlyListedWrapperIsAllowed) {
  config_.allowed_commands.push_back("bash -c ./configure");
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(bash_request("bash -c ./configure")).reply,
            PermissionReply::Once);
}

TEST_F(PermissionTest, QuotedMetacharactersAreLiteral) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(bash_request("cmake -DMSG=\"a;b\" -B /build")).reply,
            PermissionReply::Once);
  EXPECT_EQ(policy.decide(bash_request("make 'a|b'")).reply, PermissionReply::Once);
}

TEST_F(PermissionTest, UnknownProgramsAreDefaultDenied) {
  PermissionPolicy policy(config_);
  const auto decision = policy.decide(bash_request("rm -rf /"));
  EXPECT_EQ(decision.reply, PermissionReply::Reject);
  EXPECT_EQ(decision.rule_id, "default-deny");
}

TEST_F(PermissionTest, ReadsInsideTheWorktreeAreAllowed) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(path_request("read", "src/registry.cpp")).reply,
            PermissionReply::Once);
  EXPECT_EQ(policy.decide(path_request("glob", "src/**/*.hpp")).reply,
            PermissionReply::Once);
  EXPECT_EQ(policy.decide(path_request("grep", "include")).reply,
            PermissionReply::Once);
  EXPECT_EQ(policy.decide(path_request("list", ".")).reply, PermissionReply::Once);
}

TEST_F(PermissionTest, ReadsOutsideTheWorktreeAreRejected) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(path_request("read", "/etc/passwd")).rule_id,
            "path-outside-worktree");
  EXPECT_EQ(policy.decide(path_request("read", "../../etc/passwd")).rule_id,
            "path-outside-worktree");
}

TEST_F(PermissionTest, SymlinkEscapesAreRejected) {
  std::error_code ec;
  std::filesystem::create_symlink("/etc", worktree_ / "src" / "escape", ec);
  if (ec) {
    GTEST_SKIP() << "cannot create symlink: " << ec.message();
  }
  PermissionPolicy policy(config_);
  const auto decision = policy.decide(path_request("read", "src/escape/passwd"));
  EXPECT_EQ(decision.reply, PermissionReply::Reject);
  EXPECT_EQ(decision.rule_id, "path-outside-worktree");
}

TEST_F(PermissionTest, EditsAreConfinedToWritablePaths) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(path_request("edit", "src/registry.cpp")).reply,
            PermissionReply::Once);
  EXPECT_EQ(policy.decide(path_request("edit", "README.md")).rule_id,
            "path-outside-writable");
  EXPECT_EQ(policy.decide(path_request("edit", "../outside.cpp")).rule_id,
            "path-outside-worktree");
}

TEST_F(PermissionTest, ReadOnlyRolesCannotEdit) {
  config_.writable_paths.clear();
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(path_request("edit", "src/registry.cpp")).rule_id,
            "read-only-role");
}

TEST_F(PermissionTest, NetworkAndUnknownToolsAreRejected) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(path_request("webfetch", "https://example.invalid")).rule_id,
            "tool-not-allowed");
  EXPECT_EQ(policy.decide(path_request("websearch", "anything")).rule_id,
            "tool-not-allowed");
  EXPECT_EQ(policy.decide(path_request("external_directory", "/home")).rule_id,
            "tool-not-allowed");
  EXPECT_EQ(policy.decide(path_request("quantum_tool", "x")).rule_id,
            "unknown-permission-type");
}

TEST_F(PermissionTest, HarmlessToolsAreAllowed) {
  PermissionPolicy policy(config_);
  EXPECT_EQ(policy.decide(path_request("todowrite", "")).reply, PermissionReply::Once);
  EXPECT_EQ(policy.decide(path_request("question", "")).reply, PermissionReply::Once);
}

TEST_F(PermissionTest, BrokerRepliesAndCountsRejections) {
  std::vector<std::pair<std::string, PermissionReply>> replies;
  PermissionBroker broker(PermissionPolicy(config_),
                          [&replies](const PermissionRequest& request,
                                     const PermissionDecision& decision) {
                            replies.emplace_back(request.request_id, decision.reply);
                          });
  PermissionRequest request = bash_request("cmake --build /build");
  request.request_id = "per_1";
  EXPECT_EQ(broker.handle(request).reply, PermissionReply::Once);
  EXPECT_EQ(broker.rejected_in_turn(), 0u);

  for (int i = 0; i < 3; ++i) {
    PermissionRequest rejected = bash_request("rm -rf /");
    rejected.request_id = "per_reject_" + std::to_string(i);
    EXPECT_EQ(broker.handle(rejected).reply, PermissionReply::Reject);
  }
  EXPECT_EQ(broker.rejected_in_turn(), 3u);
  ASSERT_EQ(replies.size(), 4u);
  EXPECT_EQ(replies[3].second, PermissionReply::Reject);

  broker.reset_turn();
  EXPECT_EQ(broker.rejected_in_turn(), 0u);
}

TEST_F(PermissionTest, PatternsAreUsedWhenTheCommandIsMissing) {
  PermissionPolicy policy(config_);
  PermissionRequest request;
  request.type = "bash";
  request.patterns = {"cmake --version"};
  EXPECT_EQ(policy.decide(request).reply, PermissionReply::Once);
}

}  // namespace
