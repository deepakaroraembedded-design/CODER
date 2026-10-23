// Unit tests for the MCP tool surface (design section 9.1).
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "cf/mcp/tools.hpp"

namespace {

using cf::mcp::ToolRegistry;
using cf::mcp::ToolRole;
using cf::mcp::ToolResult;
using Json = nlohmann::json;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_mcp_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class McpToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = cf::store::RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Executing";
    run.created_at = 1;
    ASSERT_TRUE(store_->create_run(run).has_value());

    retriever_ = std::make_unique<cf::rag::InMemoryRetriever>();
    add_document("policy", cf::rag::DocType::Policy,
                 "# Security\n\nSecrets must be stored in the secret manager.\n");
    add_document("guide", cf::rag::DocType::Guideline,
                 "# Formatting\n\nUse clang-format for all C++ sources.\n");
    add_document("srs", cf::rag::DocType::Requirements,
                 "# Requirements\n\nREQ-010 The registry must be thread safe.\n");
    add_document("adr", cf::rag::DocType::Design,
                 "# Registry design\n\nRegistryService uses the strategy pattern.\n");
    add_document("patterns", cf::rag::DocType::Pattern,
                 "# Pattern catalog\n\nPAT-STRATEGY Strategy pattern with std::function.\n");

    cf::mcp::ToolDependencies dependencies;
    dependencies.run_id = "R12";
    dependencies.milestone_id = "M3";
    dependencies.retriever = retriever_.get();
    dependencies.store = store_.get();
    registry_ = cf::mcp::build_tool_registry(std::move(dependencies));
  }

  void TearDown() override {

    retriever_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  void add_document(const std::string& doc_id, cf::rag::DocType type,
                    const std::string& text) {
    cf::rag::Document document;
    document.doc_id = doc_id;
    document.version = "1";
    document.type = type;
    document.text = text;
    document.source = "repo://docs/" + doc_id + ".md";
    ASSERT_TRUE(retriever_->add_document(document).has_value());
  }

  std::vector<std::string> tool_names(ToolRole role) {
    std::vector<std::string> names;
    const Json listed = registry_.list_for_role(role);
    for (const auto& tool : listed["tools"]) {
      names.push_back(tool["name"].get<std::string>());
    }
    return names;
  }

  bool has_tool(ToolRole role, const std::string& name) {
    const auto names = tool_names(role);
    return std::find(names.begin(), names.end(), name) != names.end();
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::store::RunStore> store_;
  std::unique_ptr<cf::rag::InMemoryRetriever> retriever_;
  ToolRegistry registry_;
};

TEST_F(McpToolsTest, ListForRoleFiltersTools) {
  EXPECT_TRUE(has_tool(ToolRole::Coder, "submit_work"));
  EXPECT_FALSE(has_tool(ToolRole::Coder, "submit_review"));
  EXPECT_FALSE(has_tool(ToolRole::Coder, "file_defect"));
  EXPECT_TRUE(has_tool(ToolRole::Reviewer, "submit_review"));
  EXPECT_FALSE(has_tool(ToolRole::Reviewer, "submit_work"));
  EXPECT_TRUE(has_tool(ToolRole::Qa, "file_defect"));
  EXPECT_TRUE(has_tool(ToolRole::Qa, "submit_tests"));
  EXPECT_FALSE(has_tool(ToolRole::Qa, "get_pattern"));
  EXPECT_TRUE(has_tool(ToolRole::Coder, "get_pattern"));
  for (const auto role : {ToolRole::Coder, ToolRole::Reviewer, ToolRole::Qa}) {
    EXPECT_TRUE(has_tool(role, "search_guidelines"));
    EXPECT_TRUE(has_tool(role, "get_document"));
    EXPECT_TRUE(has_tool(role, "list_defects"));
  }
}

TEST_F(McpToolsTest, DeniedAndUnknownToolsReturnStructuredErrors) {
  const ToolResult denied = registry_.call(ToolRole::Coder, "submit_review", Json::object());
  EXPECT_TRUE(denied.is_error);
  EXPECT_EQ(denied.structured["code"], "TOOL_DENIED");

  const ToolResult unknown = registry_.call(ToolRole::Qa, "nonsense", Json::object());
  EXPECT_TRUE(unknown.is_error);
  EXPECT_EQ(unknown.structured["code"], "TOOL_NOT_FOUND");
}

TEST_F(McpToolsTest, InvalidArgumentsAreRejected) {
  const ToolResult missing =
      registry_.call(ToolRole::Coder, "submit_work", Json::object());
  EXPECT_TRUE(missing.is_error);
  EXPECT_EQ(missing.structured["code"], "TOOL_INVALID_ARGUMENTS");
  EXPECT_NE(missing.text.find("summary"), std::string::npos);

  const ToolResult wrong_type = registry_.call(
      ToolRole::Coder, "search_guidelines", Json{{"query", 42}});
  EXPECT_TRUE(wrong_type.is_error);
  EXPECT_EQ(wrong_type.structured["code"], "TOOL_INVALID_ARGUMENTS");

  const ToolResult unexpected = registry_.call(
      ToolRole::Coder, "search_guidelines",
      Json{{"query", "secrets"}, {"surprise", true}});
  EXPECT_TRUE(unexpected.is_error);
  EXPECT_EQ(unexpected.structured["code"], "TOOL_INVALID_ARGUMENTS");
}

TEST_F(McpToolsTest, SearchGuidelinesReturnsHits) {
  const ToolResult result = registry_.call(
      ToolRole::Coder, "search_guidelines", Json{{"query", "secret manager"}});
  ASSERT_FALSE(result.is_error) << result.text;
  ASSERT_TRUE(result.structured["hits"].is_array());
  ASSERT_FALSE(result.structured["hits"].empty());
  EXPECT_EQ(result.structured["hits"][0]["doc_id"], "policy");
  EXPECT_FALSE(result.structured["hits"][0]["citation"].get<std::string>().empty());
}

TEST_F(McpToolsTest, GetPatternReadsTheCuratedCatalog) {
  // The curated catalog is authoritative (design section 6.2), including the
  // conformance check ids; the retrieval index stays the fallback.
  auto entry = cf::rag::parse_pattern_entry(R"yaml(
id: PAT-STRATEGY
name: Strategy
level: component
use_when: ["several interchangeable algorithms"]
company_notes: "Prefer std::function."
conformance_checks:
  cpp: cf-pattern-strategy
)yaml",
                                            "strategy.yaml");
  ASSERT_TRUE(entry.has_value()) << entry.error().message;
  std::vector<cf::rag::PatternEntry> catalog = {*entry};

  cf::mcp::ToolDependencies dependencies;
  dependencies.run_id = "R12";
  dependencies.retriever = retriever_.get();
  dependencies.store = store_.get();
  dependencies.patterns = &catalog;
  const ToolRegistry with_catalog =
      cf::mcp::build_tool_registry(std::move(dependencies));

  const ToolResult found = with_catalog.call(
      ToolRole::Coder, "get_pattern", Json{{"pattern_id", "PAT-STRATEGY"}});
  ASSERT_FALSE(found.is_error) << found.text;
  EXPECT_EQ(found.structured["name"], "Strategy");
  EXPECT_EQ(found.structured["conformance_checks"]["cpp"], "cf-pattern-strategy");
  EXPECT_NE(found.text.find("Prefer std::function"), std::string::npos);

  const ToolResult missing = with_catalog.call(
      ToolRole::Coder, "get_pattern", Json{{"pattern_id", "PAT-NOPE"}});
  EXPECT_TRUE(missing.is_error);
  EXPECT_EQ(missing.structured["code"], "TOOL_NOT_FOUND");
}

TEST_F(McpToolsTest, SearchUsesTheRunContextForScopedDocuments) {
  // A product-scoped approved design (design section 6.2 step 7) applies only
  // when the MCP server carries the run's product context.
  cf::rag::Document design;
  design.doc_id = "design-PLAN-A-R1";
  design.version = "1";
  design.type = cf::rag::DocType::Design;
  design.status = "approved";
  design.scope = "product";
  design.scope_value = "Y";
  design.text = "# Design\n\nZebraCache uses a write-through layer.\n";
  design.source = "cf://runs/R1";
  ASSERT_TRUE(retriever_->add_document(design).has_value());

  cf::mcp::ToolDependencies dependencies;
  dependencies.run_id = "R12";
  dependencies.context.product = "Y";
  dependencies.retriever = retriever_.get();
  dependencies.store = store_.get();
  const ToolRegistry scoped =
      cf::mcp::build_tool_registry(std::move(dependencies));

  const ToolResult with_context = scoped.call(
      ToolRole::Coder, "search_guidelines", Json{{"query", "ZebraCache"}});
  ASSERT_FALSE(with_context.is_error) << with_context.text;
  ASSERT_FALSE(with_context.structured["hits"].empty());
  EXPECT_EQ(with_context.structured["hits"][0]["doc_id"], "design-PLAN-A-R1");

  // Without the product context the scoped document does not apply.
  const ToolResult without_context = registry_.call(
      ToolRole::Coder, "search_guidelines", Json{{"query", "ZebraCache"}});
  ASSERT_FALSE(without_context.is_error) << without_context.text;
  EXPECT_TRUE(without_context.structured["hits"].empty());
}

TEST_F(McpToolsTest, SearchGuidelinesHonoursTypeFilter) {
  const ToolResult result = registry_.call(
      ToolRole::Coder, "search_guidelines",
      Json{{"query", "registry"}, {"types", Json::array({"design"})}});
  ASSERT_FALSE(result.is_error) << result.text;
  ASSERT_TRUE(result.structured["hits"].is_array());
  ASSERT_FALSE(result.structured["hits"].empty());
  for (const auto& hit : result.structured["hits"]) {
    EXPECT_EQ(hit["type"], "design");
  }

  const ToolResult bad_type = registry_.call(
      ToolRole::Coder, "search_guidelines",
      Json{{"query", "x"}, {"types", Json::array({"blog"})}});
  EXPECT_TRUE(bad_type.is_error);
  EXPECT_EQ(bad_type.structured["code"], "TOOL_INVALID_ARGUMENTS");
}

TEST_F(McpToolsTest, GetDocumentFoundAndMissing) {
  const ToolResult found =
      registry_.call(ToolRole::Qa, "get_document", Json{{"doc_id", "policy"}});
  ASSERT_FALSE(found.is_error) << found.text;
  EXPECT_EQ(found.structured["version"], "1");
  EXPECT_NE(found.structured["text"].get<std::string>().find("secret manager"),
            std::string::npos);

  // Models often copy the "doc_id@version" form rendered in search hits.
  const ToolResult versioned = registry_.call(
      ToolRole::Qa, "get_document", Json{{"doc_id", "policy@1"}});
  ASSERT_FALSE(versioned.is_error) << versioned.text;
  EXPECT_EQ(versioned.structured["doc_id"], "policy");
  EXPECT_EQ(versioned.structured["version"], "1");

  const ToolResult missing =
      registry_.call(ToolRole::Qa, "get_document", Json{{"doc_id", "nope"}});
  EXPECT_TRUE(missing.is_error);
  EXPECT_EQ(missing.structured["code"], "TOOL_NOT_FOUND");
}

TEST_F(McpToolsTest, GetRequirementFindsById) {
  const ToolResult found =
      registry_.call(ToolRole::Reviewer, "get_requirement", Json{{"req_id", "REQ-010"}});
  ASSERT_FALSE(found.is_error) << found.text;
  EXPECT_EQ(found.structured["hits"][0]["doc_id"], "srs");

  const ToolResult missing =
      registry_.call(ToolRole::Reviewer, "get_requirement", Json{{"req_id", "REQ-999"}});
  EXPECT_TRUE(missing.is_error);
  EXPECT_EQ(missing.structured["code"], "TOOL_NOT_FOUND");
}

TEST_F(McpToolsTest, GetPatternFindsById) {
  const ToolResult found = registry_.call(ToolRole::Coder, "get_pattern",
                                           Json{{"pattern_id", "PAT-STRATEGY"}});
  ASSERT_FALSE(found.is_error) << found.text;
  EXPECT_EQ(found.structured["hits"][0]["doc_id"], "patterns");

  const ToolResult missing = registry_.call(ToolRole::Coder, "get_pattern",
                                             Json{{"pattern_id", "PAT-NOPE"}});
  EXPECT_TRUE(missing.is_error);
  EXPECT_EQ(missing.structured["code"], "TOOL_NOT_FOUND");
}

TEST_F(McpToolsTest, LookupReferenceReportsCoverage) {
  const ToolResult not_covered = registry_.call(
      ToolRole::Coder, "lookup_reference", Json{{"query", "vector push_back"}});
  ASSERT_FALSE(not_covered.is_error);
  EXPECT_EQ(not_covered.structured["covered"], false);
  EXPECT_NE(not_covered.text.find("not covered"), std::string::npos);

  cf::rag::Document reference;
  reference.doc_id = "cppreference";
  reference.version = "2026-09";
  reference.type = cf::rag::DocType::ExternalReference;
  reference.text = "# std::vector\n\npush_back appends an element.\n";
  ASSERT_TRUE(retriever_->add_document(reference).has_value());

  const ToolResult covered = registry_.call(
      ToolRole::Coder, "lookup_reference",
      Json{{"query", "vector push_back"}, {"version", "2026-09"}});
  ASSERT_FALSE(covered.is_error);
  EXPECT_EQ(covered.structured["covered"], true);
  EXPECT_FALSE(covered.structured["hits"].empty());

  const ToolResult wrong_version = registry_.call(
      ToolRole::Coder, "lookup_reference",
      Json{{"query", "vector push_back"}, {"version", "1999-01"}});
  ASSERT_FALSE(wrong_version.is_error);
  EXPECT_EQ(wrong_version.structured["covered"], false);
}

TEST_F(McpToolsTest, GetGateResultsUsesTheStore) {
  cf::store::GateResultRow row;
  row.run_id = "R12";
  row.milestone_id = "M3";
  row.commit_sha = "a1b2c3d";
  row.gate = "build";
  row.passed = true;
  row.at = 10;
  ASSERT_TRUE(store_->record_gate_result(row).has_value());

  const ToolResult latest =
      registry_.call(ToolRole::Reviewer, "get_gate_results", Json::object());
  ASSERT_FALSE(latest.is_error) << latest.text;
  ASSERT_TRUE(latest.structured["gates"].is_array());
  ASSERT_EQ(latest.structured["gates"].size(), 1u);
  EXPECT_EQ(latest.structured["gates"][0]["gate"], "build");

  const ToolResult missing = registry_.call(
      ToolRole::Reviewer, "get_gate_results", Json{{"commit", "deadbeef"}});
  EXPECT_TRUE(missing.is_error);
  EXPECT_EQ(missing.structured["code"], "TOOL_NOT_FOUND");
}

TEST_F(McpToolsTest, FileDefectDeduplicatesByFingerprint) {
  const Json defect = {{"test_id", "RegistryTest.ConcurrentRegister"},
                       {"severity", "high"},
                       {"summary", "Duplicate agent ids"},
                       {"category", "concurrency"}};

  const ToolResult first =
      registry_.call(ToolRole::Qa, "file_defect", Json{{"defect_json", defect}});
  ASSERT_FALSE(first.is_error) << first.text;
  EXPECT_EQ(first.structured["deduplicated"], false);
  const std::string id = first.structured["id"].get<std::string>();
  EXPECT_EQ(id, "D-1");

  const ToolResult second =
      registry_.call(ToolRole::Qa, "file_defect", Json{{"defect_json", defect}});
  ASSERT_FALSE(second.is_error) << second.text;
  EXPECT_EQ(second.structured["deduplicated"], true);
  EXPECT_EQ(second.structured["id"], id);

  const auto stored = store_->get_defect(id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->status, "REOPENED");

  const ToolResult listed =
      registry_.call(ToolRole::Coder, "list_defects", Json::object());
  ASSERT_FALSE(listed.is_error);
  ASSERT_EQ(listed.structured["defects"].size(), 1u);
  EXPECT_EQ(listed.structured["defects"][0]["id"], id);
}

TEST_F(McpToolsTest, DisputeTestRecordsADisputedDefect) {
  const ToolResult first = registry_.call(
      ToolRole::Coder, "dispute_test",
      Json{{"test_id", "tests/rb_interface.cpp"},
           {"rationale", "decltype of an overloaded member cannot compile"}});
  ASSERT_FALSE(first.is_error) << first.text;
  EXPECT_EQ(first.structured["status"], "DISPUTED");
  EXPECT_EQ(first.structured["disputes"], 1);
  EXPECT_EQ(first.structured["repeated"], false);
  const std::string id = first.structured["id"].get<std::string>();

  const auto stored = store_->get_defect(id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->status, "DISPUTED");
  EXPECT_EQ(stored->fingerprint, "test-dispute:tests/rb_interface.cpp");

  // Re-disputing before QA decided reuses the defect without counting a
  // repeat: the escalation only applies after a QA rejection.
  const ToolResult second = registry_.call(
      ToolRole::Coder, "dispute_test",
      Json{{"test_id", "tests/rb_interface.cpp"}, {"rationale", "same problem"}});
  ASSERT_FALSE(second.is_error) << second.text;
  EXPECT_EQ(second.structured["id"], id);
  EXPECT_EQ(second.structured["disputes"], 1);
  EXPECT_EQ(second.structured["repeated"], false);

  // The driver marks the dispute decided after the QA decision; disputing the
  // same test again then counts as a repeat and escalates (design section 12).
  auto after_decision = store_->get_defect(id);
  ASSERT_TRUE(after_decision.has_value());
  Json body = Json::parse(after_decision->body_json);
  body["decided"] = true;
  after_decision->body_json = body.dump();
  ASSERT_TRUE(store_->upsert_defect(*after_decision).has_value());
  const ToolResult third = registry_.call(
      ToolRole::Coder, "dispute_test",
      Json{{"test_id", "tests/rb_interface.cpp"}, {"rationale", "still broken"}});
  ASSERT_FALSE(third.is_error) << third.text;
  EXPECT_EQ(third.structured["id"], id);
  EXPECT_EQ(third.structured["disputes"], 2);
  EXPECT_EQ(third.structured["repeated"], true);

  // QA cannot dispute tests, and QA's verify_defect resolves the dispute.
  const ToolResult denied = registry_.call(
      ToolRole::Qa, "dispute_test",
      Json{{"test_id", "tests/x.cpp"}, {"rationale", "nope"}});
  EXPECT_TRUE(denied.is_error);
  EXPECT_EQ(denied.structured["code"], "TOOL_DENIED");

  const ToolResult verified = registry_.call(
      ToolRole::Qa, "verify_defect", Json{{"id", id}, {"result", "VERIFIED"}});
  ASSERT_FALSE(verified.is_error) << verified.text;
  const auto resolved = store_->get_defect(id);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->status, "VERIFIED");
}

TEST_F(McpToolsTest, VerifyDefectUpdatesStatus) {
  const Json defect = {{"summary", "Flaky test"}, {"severity", "minor"}};
  const ToolResult filed =
      registry_.call(ToolRole::Qa, "file_defect", Json{{"defect_json", defect}});
  ASSERT_FALSE(filed.is_error);
  const std::string id = filed.structured["id"].get<std::string>();

  const ToolResult verified =
      registry_.call(ToolRole::Qa, "verify_defect", Json{{"id", id}, {"result", "VERIFIED"}});
  ASSERT_FALSE(verified.is_error) << verified.text;
  const auto stored = store_->get_defect(id);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->status, "VERIFIED");

  const ToolResult bad_result =
      registry_.call(ToolRole::Qa, "verify_defect", Json{{"id", id}, {"result", "MAYBE"}});
  EXPECT_TRUE(bad_result.is_error);
  EXPECT_EQ(bad_result.structured["code"], "TOOL_INVALID_ARGUMENTS");
}

TEST_F(McpToolsTest, SubmissionToolsAcceptValidPayloads) {
  const ToolResult work = registry_.call(
      ToolRole::Coder, "submit_work",
      Json{{"summary", "implemented registry"}, {"defect_ids", Json::array()}});
  ASSERT_FALSE(work.is_error) << work.text;
  EXPECT_EQ(work.structured["accepted"], true);

  const ToolResult review = registry_.call(
      ToolRole::Reviewer, "submit_review",
      Json{{"verdict_json", Json{{"verdict", "APPROVED"}}}});
  ASSERT_FALSE(review.is_error) << review.text;

  const ToolResult tests = registry_.call(
      ToolRole::Qa, "submit_tests",
      Json{{"plan_ref", "qa/plan/M3.yaml"}, {"summary", "wrote 12 tests"}});
  ASSERT_FALSE(tests.is_error) << tests.text;
}

TEST_F(McpToolsTest, UnavailableToolsReturnStructuredErrors) {
  const ToolResult context = registry_.call(
      ToolRole::Coder, "get_context", Json{{"task_id", "t1"}, {"token_budget", 4000}});
  EXPECT_TRUE(context.is_error);
  EXPECT_EQ(context.structured["code"], "TOOL_UNAVAILABLE");

  const ToolResult symbols =
      registry_.call(ToolRole::Reviewer, "find_symbol", Json{{"name", "Registry"}});
  EXPECT_TRUE(symbols.is_error);
  EXPECT_EQ(symbols.structured["code"], "TOOL_UNAVAILABLE");
}

// find_symbol/get_callers backed by a real symbol index (design sections
// 6.5, 9.1).
class McpCodeIntelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = cf::codeintel::SymbolIndex::open(dir_ / "index.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    symbols_ = std::move(*opened);

    cf::codeintel::FileIndex file;
    file.record = cf::codeintel::FileRecord{"src/registry.c", "h1"};
    cf::codeintel::Symbol symbol;
    symbol.name = "registry_open";
    symbol.qualified = "registry_open";
    symbol.kind = cf::codeintel::SymbolKind::Function;
    symbol.file = "src/registry.c";
    symbol.line = 42;
    symbol.signature = "int registry_open(const char* path)";
    symbol.is_definition = true;
    file.symbols = {symbol};
    file.calls = {cf::codeintel::CallEdge{"main", "registry_open", "src/main.c", 12}};
    ASSERT_TRUE(symbols_->replace_file(file).has_value());

    cf::mcp::ToolDependencies dependencies;
    dependencies.run_id = "R12";
    dependencies.symbols = symbols_.get();
    registry_ = cf::mcp::build_tool_registry(std::move(dependencies));
  }

  void TearDown() override {
    symbols_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::unique_ptr<cf::codeintel::SymbolIndex> symbols_;
  cf::mcp::ToolRegistry registry_;
};

TEST_F(McpCodeIntelTest, FindSymbolReadsTheIndex) {
  const ToolResult found = registry_.call(ToolRole::Coder, "find_symbol",
                                          Json{{"name", "registry_open"}});
  ASSERT_FALSE(found.is_error) << found.text;
  ASSERT_EQ(found.structured["symbols"].size(), 1u);
  const auto& row = found.structured["symbols"].at(0);
  EXPECT_EQ(row["name"], "registry_open");
  EXPECT_EQ(row["kind"], "function");
  EXPECT_EQ(row["file"], "src/registry.c");
  EXPECT_EQ(row["line"], 42);
  EXPECT_TRUE(row["is_definition"]);
  EXPECT_NE(found.text.find("src/registry.c:42"), std::string::npos);

  const ToolResult missing =
      registry_.call(ToolRole::Coder, "find_symbol", Json{{"name", "nope"}});
  ASSERT_FALSE(missing.is_error) << missing.text;
  EXPECT_TRUE(missing.structured["symbols"].empty());
  EXPECT_NE(missing.text.find("no symbol"), std::string::npos);
}

TEST_F(McpCodeIntelTest, GetCallersReadsTheIndex) {
  const ToolResult result = registry_.call(ToolRole::Qa, "get_callers",
                                           Json{{"symbol", "registry_open"}});
  ASSERT_FALSE(result.is_error) << result.text;
  ASSERT_EQ(result.structured["callers"].size(), 1u);
  const auto& row = result.structured["callers"].at(0);
  EXPECT_EQ(row["caller"], "main");
  EXPECT_EQ(row["file"], "src/main.c");
  EXPECT_EQ(row["line"], 12);
  EXPECT_NE(result.text.find("main"), std::string::npos);

  const ToolResult none =
      registry_.call(ToolRole::Qa, "get_callers", Json{{"symbol", "orphan"}});
  ASSERT_FALSE(none.is_error) << none.text;
  EXPECT_TRUE(none.structured["callers"].empty());
}

TEST(McpValidationTest, ValidatorChecksSchemaRules) {
  const Json schema = Json{{"type", "object"},
                           {"properties",
                            Json{{"name", Json{{"type", "string"}}},
                                 {"tags", Json{{"type", "array"},
                                               {"items", Json{{"type", "string"}}}}}}},
                           {"required", Json::array({"name"})},
                           {"additionalProperties", false}};
  EXPECT_FALSE((cf::mcp::validate_arguments(schema, Json{{"name", "x"}}).has_value()));
  EXPECT_TRUE((cf::mcp::validate_arguments(schema, Json::object()).has_value()));
  EXPECT_TRUE((cf::mcp::validate_arguments(schema, Json{{"name", 1}}).has_value()));
  EXPECT_TRUE((cf::mcp::validate_arguments(schema, Json{{"name", "x"}, {"extra", 1}})
                   .has_value()));
  EXPECT_TRUE((cf::mcp::validate_arguments(
                   schema, Json{{"name", "x"}, {"tags", Json::array({1})}})
                   .has_value()));
  EXPECT_FALSE((cf::mcp::validate_arguments(
                    schema, Json{{"name", "x"}, {"tags", Json::array({"a"})}})
                    .has_value()));
}

}  // namespace
