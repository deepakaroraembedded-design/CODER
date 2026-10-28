// Structural tests for the company Semgrep rule sets (design section 6.7.2).
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace {

std::vector<std::string> rule_files() {
  return {std::string(CF_SOURCE_DIR) + "/rules/semgrep/python.yaml",
          std::string(CF_SOURCE_DIR) + "/rules/semgrep/typescript.yaml",
          std::string(CF_SOURCE_DIR) + "/rules/semgrep/javascript.yaml",
          std::string(CF_SOURCE_DIR) + "/rules/semgrep/java.yaml",
          std::string(CF_SOURCE_DIR) + "/rules/semgrep/go.yaml"};
}

TEST(SemgrepRulesTest, AllCompanyRulesAreWellFormed) {
  std::set<std::string> ids;
  for (const auto& path : rule_files()) {
    const YAML::Node document = YAML::LoadFile(path);
    const YAML::Node rules = document["rules"];
    ASSERT_TRUE(rules.IsSequence()) << path;
    ASSERT_FALSE(rules.size() == 0) << path;
    for (const auto& rule : rules) {
      const std::string id = rule["id"].as<std::string>();
      EXPECT_EQ(id.rfind("swt.", 0), 0u) << path;
      EXPECT_TRUE(ids.insert(id).second) << "duplicate rule id " << id;
      EXPECT_EQ(rule["severity"].as<std::string>(), "ERROR") << id;
      EXPECT_FALSE(rule["message"].as<std::string>().empty()) << id;
      ASSERT_TRUE(rule["languages"].IsSequence()) << id;
      EXPECT_FALSE(rule["languages"].size() == 0) << id;
      EXPECT_TRUE(rule["pattern"] || rule["patterns"] || rule["pattern-either"])
          << id;
    }
  }
}

}  // namespace
