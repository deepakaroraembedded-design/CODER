// Unit tests for the dependency licence checks (design sections 6.7.1 gate
// 10, 10.4.6 check 6).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cf/github/licenses.hpp"

namespace {

using cf::github::check_licenses;
using cf::github::dependencies_in_manifest;
using cf::github::LicenseDatabase;
using cf::github::LicensePolicy;

TEST(LicensesTest, ParsesVcpkgJson) {
  const std::string manifest = R"({
    "name": "x",
    "dependencies": ["fmt", {"name": "cpr", "features": ["ssl"]}, "spdlog"]
  })";
  auto deps = dependencies_in_manifest("vcpkg.json", manifest);
  ASSERT_TRUE(deps.has_value());
  EXPECT_EQ(*deps, (std::vector<std::string>{"fmt", "cpr", "spdlog"}));
}

TEST(LicensesTest, ParsesRequirementsTxt) {
  const std::string manifest =
      "# comment\n"
      "requests==2.31.0\n"
      "numpy>=1.26 ; python_version >= \"3.10\"\n"
      "uvicorn[standard]==0.29.0\n"
      "-r other.txt\n"
      "\n";
  auto deps = dependencies_in_manifest("requirements.txt", manifest);
  ASSERT_TRUE(deps.has_value());
  EXPECT_EQ(*deps, (std::vector<std::string>{"requests", "numpy", "uvicorn"}));
}

TEST(LicensesTest, ParsesPackageJson) {
  const std::string manifest = R"({
    "name": "x",
    "dependencies": {"react": "^18.0.0"},
    "devDependencies": {"vitest": "^1.0.0"}
  })";
  auto deps = dependencies_in_manifest("package.json", manifest);
  ASSERT_TRUE(deps.has_value());
  EXPECT_EQ(deps->size(), 2u);
  EXPECT_NE(std::find(deps->begin(), deps->end(), "react"), deps->end());
  EXPECT_NE(std::find(deps->begin(), deps->end(), "vitest"), deps->end());
}

TEST(LicensesTest, ParsesGoMod) {
  const std::string manifest =
      "module example.com/x\n\n"
      "require github.com/stretchr/testify v1.9.0\n\n"
      "require (\n"
      "\tgolang.org/x/text v0.14.0\n"
      "\tgithub.com/google/uuid v1.6.0 // indirect\n"
      ")\n";
  auto deps = dependencies_in_manifest("go.mod", manifest);
  ASSERT_TRUE(deps.has_value());
  EXPECT_EQ(*deps, (std::vector<std::string>{"github.com/stretchr/testify",
                                             "golang.org/x/text",
                                             "github.com/google/uuid"}));
}

TEST(LicensesTest, UnknownManifestsYieldNothing) {
  auto deps = dependencies_in_manifest("Makefile", "all:\n\techo hi\n");
  ASSERT_TRUE(deps.has_value());
  EXPECT_TRUE(deps->empty());
}

TEST(LicensesTest, LoadsTheDatabaseAndLooksUp) {
  auto database = LicenseDatabase::load_string(
      "licenses:\n  fmt: MIT\n  openssl: Apache-2.0\n");
  ASSERT_TRUE(database.has_value()) << database.error().message;
  EXPECT_EQ(database->lookup("fmt"), "MIT");
  EXPECT_EQ(database->lookup("openssl"), "Apache-2.0");
  EXPECT_EQ(database->lookup("nope"), "");

  auto broken = LicenseDatabase::load_string("other: {}\n");
  ASSERT_FALSE(broken.has_value());
  EXPECT_EQ(broken.error().code, cf::github::error_codes::kLicenseDbInvalid);
}

TEST(LicensesTest, ChecksAgainstTheAllowList) {
  auto database = LicenseDatabase::load_string(
      "licenses:\n  fmt: MIT\n  gpl-thing: GPL-3.0-only\n");
  ASSERT_TRUE(database.has_value());
  LicensePolicy policy;  // the default allow-list

  const auto allowed = check_licenses({"fmt"}, *database, policy);
  EXPECT_TRUE(allowed.empty());

  const auto disallowed = check_licenses({"gpl-thing"}, *database, policy);
  ASSERT_EQ(disallowed.size(), 1u);
  EXPECT_EQ(disallowed[0].license, "GPL-3.0-only");
  EXPECT_NE(disallowed[0].reason.find("not on the licence allow-list"),
            std::string::npos);

  const auto unknown = check_licenses({"mystery"}, *database, policy);
  ASSERT_EQ(unknown.size(), 1u);
  EXPECT_NE(unknown[0].reason.find("unknown licence"), std::string::npos);
}

TEST(LicensesTest, LicenseComparisonIsCaseInsensitive) {
  auto database = LicenseDatabase::load_string("licenses:\n  x: mit\n");
  ASSERT_TRUE(database.has_value());
  EXPECT_TRUE(check_licenses({"x"}, *database, LicensePolicy{}).empty());
}

}  // namespace
