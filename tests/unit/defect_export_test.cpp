// Unit tests for the defect export to GitHub Issues (design section 6.10).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/orch/defect_export.hpp"
#include "cf/orch/github_handlers.hpp"
#include "fake_http_transport.hpp"

namespace {

using cf::github::Client;
using cf::github::ClientOptions;
using cf::orch::DefectExportConfig;
using cf::orch::DefectExporter;
using cf::orch::Dispatcher;
using cf::orch::GithubHandlerContext;
using cf::orch::IssueCreateHandler;
using cf::orch::IssueUpdateHandler;
using cf::store::DefectRecord;
using cf::store::RunStore;
using cf::test::ScriptedHttpTransport;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_defect_export_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

class DefectExportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto opened = RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Executing";
    run.created_at = now_;
    ASSERT_TRUE(store_->create_run(run).has_value());
    auto lease = store_->acquire_lease("R12", "test", std::chrono::seconds(60), now_);
    ASSERT_TRUE(lease.has_value());
    epoch_ = lease->epoch;

    transport_ = std::make_unique<ScriptedHttpTransport>();
    ClientOptions options;
    options.api_base = "https://api.github.test";
    options.token = "ghs_test";
    client_ = std::make_unique<Client>(options, *transport_);

    GithubHandlerContext context;
    context.client = client_.get();
    context.owner = "o";
    context.repo = "r";
    dispatcher_ = std::make_unique<Dispatcher>(*store_, "R12", "test");
    dispatcher_->register_handler(std::make_unique<IssueCreateHandler>(context));
    dispatcher_->register_handler(std::make_unique<IssueUpdateHandler>(context));
  }

  void TearDown() override {
    dispatcher_.reset();
    client_.reset();
    transport_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  DefectRecord make_defect(const std::string& id, const std::string& status,
                           const std::string& repro = "ctest -R Flaky --repeat 50") {
    DefectRecord defect;
    defect.id = id;
    defect.run_id = "R12";
    defect.milestone_id = "M3";
    defect.status = status;
    defect.severity = "high";
    defect.fingerprint = "sha256:fp-" + id;
    defect.body_json = nlohmann::json{{"summary", "Duplicate agent ids"},
                                      {"severity", "high"},
                                      {"category", "concurrency"},
                                      {"repro", repro},
                                      {"requirement_ids", nlohmann::json::array({"REQ-010"})}}
                           .dump();
    return defect;
  }

  std::unique_ptr<DefectExporter> make_exporter(bool restricted = false) {
    DefectExportConfig config;
    config.owner = "o";
    config.repo = "r";
    config.restricted = restricted;
    config.report_base = "artifact://runs/R12";
    config.client = client_.get();
    return std::make_unique<DefectExporter>(*store_, *dispatcher_, std::move(config));
  }

  std::filesystem::path dir_;
  std::unique_ptr<RunStore> store_;
  std::unique_ptr<ScriptedHttpTransport> transport_;
  std::unique_ptr<Client> client_;
  std::unique_ptr<Dispatcher> dispatcher_;
  cf::store::UnixTime now_ = cf::store::unix_now();
  uint64_t epoch_ = 0;
};

TEST_F(DefectExportTest, FilesAnIssueWithLabelsAndFingerprintMarker) {
  ASSERT_TRUE(store_->upsert_defect(make_defect("D-1", "OPEN")).has_value());
  transport_->script(200, "[]");  // no existing issue
  transport_->script(201,
                     R"({"number": 5, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");

  auto exporter = make_exporter();
  auto synced = exporter->sync("R12", epoch_, now_);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;
  EXPECT_EQ(*synced, 1u);

  ASSERT_EQ(transport_->requests().size(), 2u);
  const auto& created = transport_->requests()[1];
  EXPECT_EQ(created.method, "POST");
  EXPECT_NE(created.body.find("cf-defect"), std::string::npos);
  EXPECT_NE(created.body.find("severity:high"), std::string::npos);
  EXPECT_NE(created.body.find("milestone:M3"), std::string::npos);
  EXPECT_NE(created.body.find("requirement:REQ-010"), std::string::npos);
  EXPECT_NE(created.body.find("<!-- cf-fingerprint: sha256:fp-D-1 -->"),
            std::string::npos);
  EXPECT_NE(created.body.find("ctest -R Flaky"), std::string::npos);

  // The operation is confirmed and carries the defect id for later lookups.
  auto operations = store_->list_operations("R12", {cf::store::op_state::kConfirmed});
  ASSERT_TRUE(operations.has_value());
  ASSERT_EQ(operations->size(), 1u);
  EXPECT_EQ(operations->front().kind, "issue_create");
  EXPECT_NE(operations->front().input_json.find("D-1"), std::string::npos);

  // A second sync dispatches nothing new; it only polls the issue to detect
  // a human closure (which the unscripted transport answers with an error,
  // skipped best-effort).
  const auto again = exporter->sync("R12", epoch_, now_);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, 0u);
  EXPECT_EQ(transport_->requests().size(), 3u);
  EXPECT_EQ(transport_->requests()[2].method, "GET");
}

TEST_F(DefectExportTest, VerifiedDefectClosesTheIssue) {
  ASSERT_TRUE(store_->upsert_defect(make_defect("D-1", "OPEN")).has_value());
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 5, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");
  auto exporter = make_exporter();
  ASSERT_TRUE(exporter->sync("R12", epoch_, now_).has_value());

  ASSERT_TRUE(store_->set_defect_status("D-1", "VERIFIED", 0).has_value());
  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "b", "state": "open"})");  // pull-back check
  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "closed", "state": "closed"})");
  const auto synced = exporter->sync("R12", epoch_, now_);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;
  EXPECT_EQ(*synced, 1u);

  bool closed = false;
  for (const auto& request : transport_->requests()) {
    if (request.method == "PATCH" &&
        request.body.find("\"state\":\"closed\"") != std::string::npos) {
      closed = true;
    }
  }
  EXPECT_TRUE(closed);
}

TEST_F(DefectExportTest, HumanClosureBecomesWontFix) {
  ASSERT_TRUE(store_->upsert_defect(make_defect("D-1", "OPEN")).has_value());
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 5, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");
  auto exporter = make_exporter();
  ASSERT_TRUE(exporter->sync("R12", epoch_, now_).has_value());

  // A person closed the issue as "not planned".
  transport_->script(200, R"({"number": 5, "html_url": "u", "title": "t",
                               "body": "wont fix", "state": "closed"})");
  const auto synced = exporter->sync("R12", epoch_, now_);
  ASSERT_TRUE(synced.has_value()) << synced.error().message;

  const auto stored = store_->get_defect("D-1");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->status, "WONT_FIX");
}

TEST_F(DefectExportTest, RestrictedRunsOmitReproAndEvidence) {
  ASSERT_TRUE(store_->upsert_defect(make_defect("D-1", "OPEN")).has_value());
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 5, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");
  auto exporter = make_exporter(/*restricted=*/true);
  ASSERT_TRUE(exporter->sync("R12", epoch_, now_).has_value());
  ASSERT_GE(transport_->requests().size(), 2u);
  const std::string body = transport_->requests()[1].body;
  EXPECT_EQ(body.find("ctest -R Flaky"), std::string::npos);
  EXPECT_NE(body.find("omitted here"), std::string::npos);
  EXPECT_NE(body.find("artifact://runs/R12"), std::string::npos);
}

TEST_F(DefectExportTest, SecretsInTheReproAreRedacted) {
  ASSERT_TRUE(
      store_->upsert_defect(make_defect("D-1", "OPEN", "curl -H 'Authorization: "
                                                          "Bearer AKIAIOSFODNN7EXAMPLE'"))
                  .has_value());
  transport_->script(200, "[]");
  transport_->script(201,
                     R"({"number": 5, "html_url": "u", "title": "t", "body": "b",
                         "state": "open"})");
  auto exporter = make_exporter();
  ASSERT_TRUE(exporter->sync("R12", epoch_, now_).has_value());
  ASSERT_GE(transport_->requests().size(), 2u);
  const std::string body = transport_->requests()[1].body;
  EXPECT_EQ(body.find("AKIAIOSFODNN7EXAMPLE"), std::string::npos);
  EXPECT_NE(body.find("omitted here"), std::string::npos);
}

}  // namespace
