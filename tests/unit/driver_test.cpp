// Unit tests for the workspace milestone driver (design sections 6.6-6.13).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "cf/orch/driver.hpp"
#include "cf/orch/orchestrator.hpp"

namespace {

using cf::orch::AgentRunner;
using cf::orch::AgentTurnOutcome;
using cf::orch::AgentTurnRequest;
using cf::orch::DriverConfig;
using cf::orch::GateOutcome;
using cf::orch::MilestoneEvent;
using cf::orch::MilestoneLoop;
using cf::orch::MilestonePhase;
using cf::orch::MilestoneState;
using cf::orch::StateMachine;
using cf::orch::TurnResult;
using cf::orch::WorkspaceDriver;

std::filesystem::path make_temp_dir() {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("cf_driver_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(dir);
  return dir;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

class FakeAgentRunner final : public AgentRunner {
 public:
  using TurnFn =
      std::function<cf::Result<AgentTurnOutcome>(const AgentTurnRequest&)>;

  cf::Result<AgentTurnOutcome> run_turn(const AgentTurnRequest& request) override {
    phases.push_back(request.phase);
    briefs.push_back(request.brief);
    worktrees.push_back(request.worktree);
    if (on_turn) {
      return on_turn(request);
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    return outcome;
  }

  cf::Result<AgentTurnOutcome> run_system_validation(
      const AgentTurnRequest& request) override {
    if (on_system) {
      return on_system(request);
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.exit_criteria_met = true;
    return outcome;
  }

  TurnFn on_turn;
  TurnFn on_system;
  std::vector<MilestonePhase> phases;
  std::vector<std::string> briefs;
  std::vector<std::string> worktrees;
};

const char* kPassingProfile = R"yaml(
id: test-profile
image: test-image
languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails: { steps: [[true]] }
    format: { steps: [[true]] }
build_systems:
  cmake:
    steps: [[true]]
test_frameworks:
  gtest: { steps: [[true]], languages: [cpp] }
)yaml";

const char* kFailingBuildProfile = R"yaml(
id: test-profile
image: test-image
languages:
  cpp:
    standard_var: CMAKE_CXX_STANDARD
    guardrails: { steps: [[true]] }
    format: { steps: [[true]] }
build_systems:
  cmake:
    steps:
      - [sh, -c, "test ! -f src/bad.cpp"]
test_frameworks:
  gtest: { steps: [[true]], languages: [cpp] }
)yaml";

class DriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = make_temp_dir();
    auto created = cf::git::Repository::create(dir_ / "repo", executor_);
    ASSERT_TRUE(created.has_value()) << created.error().message;
    repository_ = std::make_unique<cf::git::Repository>(std::move(*created));

    write_file(dir_ / "repo" / "README.md", "initial\n");
    cf::git::CommitTrailers trailers;
    trailers.run_id = "R12";
    trailers.op_id = "op_init";
    ASSERT_TRUE(
        repository_->commit_paths(dir_ / "repo", {"README.md"}, "chore: init", trailers)
            .has_value());

    auto opened = cf::store::RunStore::open(dir_ / "run.db");
    ASSERT_TRUE(opened.has_value()) << opened.error().message;
    store_ = std::move(*opened);
    cf::store::RunRecord run;
    run.run_id = "R12";
    run.state = "Executing";
    run.created_at = cf::store::unix_now();
    ASSERT_TRUE(store_->create_run(run).has_value());

    plan_.plan_id = "PLAN-X";
    plan_.product = "Y";
    plan_.version = "1";
    cf::plan::MilestoneSpec milestone;
    milestone.id = "M3";
    milestone.title = "Registry";
    milestone.requirements = {"REQ-010"};
    plan_.milestones.push_back(milestone);
    plan_.target.language = "cpp";
    plan_.target.standard = "c++20";
    plan_.target.build_system = "cmake";
    plan_.target.test_framework = "gtest";
    now_ = cf::store::unix_now();
  }

  void TearDown() override {
    store_.reset();
    repository_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  DriverConfig config_for(const std::string& profile_yaml) {
    auto profile = cf::gates::load_profile_string(profile_yaml);
    EXPECT_TRUE(profile.has_value()) << profile.error().message;
    DriverConfig config;
    config.run_id = "R12";
    config.workspace_root = dir_ / "workspace";
    config.integration_branch = "cf/R12/integration";
    config.base_branch = "main";
    config.profile = profile.has_value() ? *profile : cf::gates::Profile{};
    config.profile_language = "cpp";
    config.profile_build_system = "cmake";
    config.profile_test_framework = "gtest";
    config.step_timeout = std::chrono::milliseconds(10000);
    return config;
  }

  std::filesystem::path dir_;
  cf::process::LocalProcessExecutor executor_;
  std::unique_ptr<cf::git::Repository> repository_;
  std::unique_ptr<cf::store::RunStore> store_;
  cf::plan::Plan plan_;
  cf::store::UnixTime now_ = 0;
};

TEST_F(DriverTest, FullLoopReachesDoneWithRealCommitsAndGates) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    switch (request.phase) {
      case MilestonePhase::AuthorTests:
        write_file(std::filesystem::path(request.worktree) / "tests" / "m3_test.cpp",
                   "test\n");
        break;
      case MilestonePhase::Code:
        write_file(std::filesystem::path(request.worktree) / "src" / "m3.cpp",
                   "int registry() { return 0; }\n");
        break;
      case MilestonePhase::Validate: {
        AgentTurnOutcome outcome;
        outcome.completed = true;
        outcome.exit_criteria_met = true;
        return outcome;
      }
      default:
        break;
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "done";
    return outcome;
  };

  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  ASSERT_TRUE(machine.apply("M3", MilestoneEvent::Start, now_).has_value());
  MilestoneLoop loop(*store_, machine, driver);

  const auto state = loop.drive("M3", now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, MilestoneState::Done);

  // The review ran (first gate run -> full review) and the QA tests ran.
  EXPECT_NE(std::find(runner.phases.begin(), runner.phases.end(),
                      MilestonePhase::Review),
            runner.phases.end());
  EXPECT_NE(std::find(runner.phases.begin(), runner.phases.end(),
                      MilestonePhase::AuthorTests),
            runner.phases.end());

  // The integration branch carries the merged work.
  const auto integration = driver.integration_worktree();
  EXPECT_TRUE(std::filesystem::exists(integration / "src" / "m3.cpp"));
  EXPECT_TRUE(std::filesystem::exists(integration / "tests" / "m3_test.cpp"));

  // Commits carry the CF trailers on the milestone branch.
  auto commit = repository_->find_commit_by_trailer("cf-local/R12/M3/coder",
                                                    "CF-Milestone", "M3");
  ASSERT_TRUE(commit.has_value()) << commit.error().message;

  // Gate results are recorded for the gated commit.
  auto gates = store_->latest_gate_results("R12", "M3");
  ASSERT_TRUE(gates.has_value()) << gates.error().message;
  EXPECT_FALSE(gates->empty());
  for (const auto& row : *gates) {
    EXPECT_TRUE(row.passed) << row.gate;
  }

  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->state, "Done");
  EXPECT_EQ(stored->iterations, 1u);
}

TEST_F(DriverTest, GateFailureLeadsToFixAndSecondIteration) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    const std::filesystem::path worktree(request.worktree);
    switch (request.phase) {
      case MilestonePhase::AuthorTests:
        write_file(worktree / "tests" / "m3_test.cpp", "test\n");
        break;
      case MilestonePhase::Code:
        write_file(worktree / "src" / "m3.cpp", "v1\n");
        write_file(worktree / "src" / "bad.cpp", "broken\n");
        break;
      case MilestonePhase::Fix:
        std::filesystem::remove(worktree / "src" / "bad.cpp");
        write_file(worktree / "src" / "m3.cpp", "v2\n");
        break;
      case MilestonePhase::Validate: {
        AgentTurnOutcome outcome;
        outcome.completed = true;
        outcome.exit_criteria_met = true;
        return outcome;
      }
      default:
        break;
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    return outcome;
  };

  WorkspaceDriver driver(plan_, config_for(kFailingBuildProfile), *repository_,
                         executor_, runner, store_.get());
  StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  ASSERT_TRUE(machine.apply("M3", MilestoneEvent::Start, now_).has_value());
  MilestoneLoop loop(*store_, machine, driver);

  const auto state = loop.drive("M3", now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, MilestoneState::Done);
  EXPECT_NE(std::find(runner.phases.begin(), runner.phases.end(), MilestonePhase::Fix),
            runner.phases.end());

  const auto stored = store_->get_milestone("R12", "M3");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->iterations, 2u);
}

TEST_F(DriverTest, SnapshotWithoutChangesFailsTheTurn) {
  FakeAgentRunner runner;  // completes but writes nothing
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  const TurnResult result = driver.run(MilestonePhase::Code, "M3");
  EXPECT_FALSE(result.completed);
  EXPECT_FALSE(result.detail.empty());
}

TEST_F(DriverTest, ReviewChangesRequestedPropagates) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    AgentTurnOutcome outcome;
    outcome.completed = true;
    if (request.phase == MilestonePhase::Review) {
      outcome.changes_requested = true;
    }
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  const TurnResult result = driver.run(MilestonePhase::Review, "M3");
  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.changes_requested);
}

TEST_F(DriverTest, SystemValidationDelegatesToTheRunner) {
  FakeAgentRunner runner;
  runner.on_system = [](const AgentTurnRequest&) -> cf::Result<AgentTurnOutcome> {
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.exit_criteria_met = false;
    outcome.detail = "system defects found";
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  const auto validation = driver.validate_system();
  EXPECT_TRUE(validation.completed);
  EXPECT_FALSE(validation.passed);
  EXPECT_EQ(validation.detail, "system defects found");
}

TEST_F(DriverTest, IntegrateConflictIsReported) {
  FakeAgentRunner runner;
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());

  // Integration branch has its own version of the file.
  const auto integration = driver.integration_worktree();
  ASSERT_TRUE(repository_
                  ->create_worktree(integration, "cf/R12/integration", "main")
                  .has_value());
  write_file(integration / "conflict.cpp", "integration\n");
  cf::git::CommitTrailers integration_trailers;
  integration_trailers.op_id = "op_int";
  ASSERT_TRUE(repository_
                  ->commit_paths(integration, {"conflict.cpp"}, "feat: integration",
                                 integration_trailers)
                  .has_value());

  // The milestone branch edits the same file differently.
  const auto coder = driver.worktree_for("M3", "coder");
  ASSERT_TRUE(repository_
                  ->create_worktree(coder, "cf-local/R12/M3/coder", "main")
                  .has_value());
  write_file(coder / "conflict.cpp", "coder\n");
  cf::git::CommitTrailers coder_trailers;
  coder_trailers.run_id = "R12";
  coder_trailers.milestone_id = "M3";
  coder_trailers.op_id = "op_coder";
  ASSERT_TRUE(repository_
                  ->commit_paths(coder, {"conflict.cpp"}, "feat: coder", coder_trailers)
                  .has_value());

  const TurnResult result = driver.run(MilestonePhase::Integrate, "M3");
  EXPECT_TRUE(result.completed);
  EXPECT_FALSE(result.merged);
  EXPECT_TRUE(result.conflicts);
}

TEST_F(DriverTest, OpenDefectsArePassedToTheAgent) {
  cf::store::DefectRecord defect;
  defect.id = "D-1";
  defect.run_id = "R12";
  defect.milestone_id = "M3";
  defect.status = "OPEN";
  defect.severity = "high";
  defect.body_json = "{}";
  ASSERT_TRUE(store_->upsert_defect(defect).has_value());
  cf::store::DefectRecord verified;
  verified.id = "D-2";
  verified.run_id = "R12";
  verified.milestone_id = "M3";
  verified.status = "VERIFIED";
  verified.severity = "minor";
  verified.body_json = "{}";
  ASSERT_TRUE(store_->upsert_defect(verified).has_value());

  FakeAgentRunner runner;
  std::vector<std::string> seen_defects;
  runner.on_turn = [&seen_defects](const AgentTurnRequest& request)
      -> cf::Result<AgentTurnOutcome> {
    seen_defects = request.open_defect_ids;
    write_file(std::filesystem::path(request.worktree) / "src" / "m3.cpp", "code\n");
    AgentTurnOutcome outcome;
    outcome.completed = true;
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  const TurnResult result = driver.run(MilestonePhase::Code, "M3");
  EXPECT_TRUE(result.completed) << result.detail;
  EXPECT_EQ(seen_defects, (std::vector<std::string>{"D-1"}));
}

TEST_F(DriverTest, TestDisputeDecisionRunsQaInTheCoderWorktree) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    AgentTurnOutcome outcome;
    outcome.completed = true;
    if (request.phase == MilestonePhase::AuthorTests) {
      write_file(std::filesystem::path(request.worktree) / "tests" / "m3_test.cpp",
                 "broken test\n");
    } else if (request.phase == MilestonePhase::Validate) {
      // QA decides the dispute by fixing the protected test in place.
      write_file(std::filesystem::path(request.worktree) / "tests" / "m3_test.cpp",
                 "fixed test\n");
      outcome.exit_criteria_met = true;
    }
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  ASSERT_TRUE(driver.run(MilestonePhase::AuthorTests, "M3").completed);

  // The dispute tool records the DISPUTED defect the decision turn reads.
  cf::store::DefectRecord dispute;
  dispute.id = "D-1";
  dispute.run_id = "R12";
  dispute.milestone_id = "M3";
  dispute.status = "DISPUTED";
  dispute.severity = "high";
  dispute.fingerprint = "test-dispute:tests/m3_test.cpp";
  dispute.body_json =
      R"({"test_id": "tests/m3_test.cpp", "rationale": "cannot compile", "disputes": 1})";
  ASSERT_TRUE(store_->upsert_defect(dispute).has_value());

  const std::filesystem::path coder_worktree = driver.worktree_for("M3", "coder");

  const TurnResult result = driver.run_test_dispute("M3");
  ASSERT_TRUE(result.completed) << result.detail;

  // The QA turn ran in the coder's worktree (so the next gate build sees the
  // fix), its brief carried the dispute, and the fix was committed there.
  ASSERT_EQ(runner.worktrees.back(), coder_worktree.string());
  EXPECT_NE(runner.briefs.back().find("tests/m3_test.cpp"), std::string::npos);
  EXPECT_NE(runner.briefs.back().find("cannot compile"), std::string::npos);
  const auto after = repository_->head_sha(coder_worktree);
  ASSERT_TRUE(after.has_value());
  const auto message = repository_->commit_message(*after);
  ASSERT_TRUE(message.has_value());
  EXPECT_NE(message->find("dispute decision"), std::string::npos);
  const auto committed = repository_->file_content(*after, "tests/m3_test.cpp");
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(*committed, "fixed test\n");
  const auto dirty = repository_->is_dirty(coder_worktree);
  ASSERT_TRUE(dirty.has_value());
  EXPECT_FALSE(*dirty);

  // The test change resolved the dispute: the defect is no longer open.
  const auto resolved = store_->get_defect("D-1");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->status, "VERIFIED");
}

TEST_F(DriverTest, MultiMilestoneCoderWorktreesCarryTheMergedWork) {
  // M3 depends on M1 and uses the interface M2 writes: the scheduler blocks
  // it until M2 is merged, and its coder worktree must then start from its
  // QA branch (which starts from the integration).
  plan_.milestones.clear();
  cf::plan::MilestoneSpec m1;
  m1.id = "M1";
  m1.title = "Message";
  m1.writes = {"include/message.hpp"};
  cf::plan::MilestoneSpec m2;
  m2.id = "M2";
  m2.title = "Channel";
  m2.depends_on = {"M1"};
  m2.writes = {"include/channel.hpp"};
  m2.uses = {"include/message.hpp"};
  m2.writes_interfaces = {"include/channel.hpp"};
  cf::plan::MilestoneSpec m3;
  m3.id = "M3";
  m3.title = "Consumer";
  m3.depends_on = {"M1"};
  m3.writes = {"include/consumer.hpp"};
  m3.uses = {"include/message.hpp", "include/channel.hpp"};
  plan_.milestones = {m1, m2, m3};

  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    const std::filesystem::path worktree(request.worktree);
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "done";
    switch (request.phase) {
      case MilestonePhase::AuthorTests:
        write_file(worktree / "tests" / (request.milestone_id + "_test.cpp"), "test\n");
        break;
      case MilestonePhase::Code:
        if (request.milestone_id == "M1") {
          write_file(worktree / "include" / "message.hpp", "// message\n");
        } else if (request.milestone_id == "M2") {
          write_file(worktree / "include" / "channel.hpp", "// channel\n");
        } else if (request.milestone_id == "M3") {
          write_file(worktree / "include" / "consumer.hpp", "// consumer\n");
        }
        break;
      case MilestonePhase::Validate:
        outcome.exit_criteria_met = true;
        break;
      default:
        break;
    }
    return outcome;
  };

  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  StateMachine machine(*store_, "R12");
  for (const auto& spec : plan_.milestones) {
    ASSERT_TRUE(machine.register_milestone(spec.id).has_value());
  }
  cf::orch::RunOrchestrator orchestrator(*store_, "R12", plan_, driver);
  const auto state = orchestrator.step(now_);
  ASSERT_TRUE(state.has_value()) << state.error().message;
  EXPECT_EQ(*state, cf::orch::RunState::SystemValidation);

  // Every coder worktree carries its own QA tests plus the work merged
  // before it started.
  const auto coder_worktree = driver.worktree_for("M3", "coder");
  EXPECT_TRUE(std::filesystem::exists(coder_worktree / "tests" / "M3_test.cpp"));
  EXPECT_TRUE(std::filesystem::exists(coder_worktree / "include" / "message.hpp"));
  EXPECT_TRUE(std::filesystem::exists(coder_worktree / "include" / "channel.hpp"));
}

TEST_F(DriverTest, ChangesOutsideTheDeclaredWriteSetAreRejected) {
  plan_.milestones[0].writes = {"src/registry"};

  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    write_file(std::filesystem::path(request.worktree) / "src" / "registry" / "ok.cpp",
               "ok\n");
    write_file(std::filesystem::path(request.worktree) / "src" / "elsewhere" / "bad.cpp",
               "bad\n");
    // Build artifacts are never committed and do not count as out-of-scope
    // changes (the agent builds inside its worktree).
    write_file(std::filesystem::path(request.worktree) / "build" / "artifact.o",
               "binary\n");
    AgentTurnOutcome outcome;
    outcome.completed = true;
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());

  const TurnResult result = driver.run(MilestonePhase::Code, "M3");
  EXPECT_FALSE(result.completed);
  EXPECT_NE(result.detail.find("outside the declared write set"), std::string::npos);
  EXPECT_NE(result.detail.find("src/elsewhere/bad.cpp"), std::string::npos);
  EXPECT_EQ(result.detail.find("build/artifact.o"), std::string::npos);

  // Nothing was committed.
  const auto coder_worktree = driver.worktree_for("M3", "coder");
  const auto head = repository_->head_sha(coder_worktree);
  ASSERT_TRUE(head.has_value());
  const auto message = repository_->commit_message(*head);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->find("feat(M3)"), std::string::npos);
}

TEST_F(DriverTest, ChangesInsideTheDeclaredWriteSetCommit) {
  plan_.milestones[0].writes = {"src/registry"};

  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    write_file(std::filesystem::path(request.worktree) / "src" / "registry" / "ok.cpp",
               "ok\n");
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "implemented";
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());

  const TurnResult result = driver.run(MilestonePhase::Code, "M3");
  EXPECT_TRUE(result.completed) << result.detail;
  const auto coder_worktree = driver.worktree_for("M3", "coder");
  const auto head = repository_->head_sha(coder_worktree);
  ASSERT_TRUE(head.has_value());
  const auto message = repository_->commit_message(*head);
  ASSERT_TRUE(message.has_value());
  EXPECT_NE(message->find("feat(M3)"), std::string::npos);
}

TEST_F(DriverTest, RevalidationGatesTheIntegrationAndReintegratingIsANoOp) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    if (request.phase == MilestonePhase::Code) {
      write_file(std::filesystem::path(request.worktree) / "include" / "core.hpp",
                 "// core\n");
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "implemented";
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());

  ASSERT_TRUE(driver.run(MilestonePhase::Code, "M3").completed);
  const TurnResult integrated = driver.run(MilestonePhase::Integrate, "M3");
  ASSERT_TRUE(integrated.completed) << integrated.detail;
  EXPECT_TRUE(integrated.interface_changed);
  EXPECT_EQ(driver.integration_interfaces("M3"),
            (std::vector<std::string>{"include/core.hpp"}));

  // A dependent's revalidation gates the integration head, not the (stale)
  // milestone worktree, and forces a full review (design section 6.13.3).
  driver.mark_revalidating("M3");
  const GateOutcome gates = driver.run_gates("M3");
  EXPECT_TRUE(gates.passed);
  EXPECT_TRUE(gates.review_context.dependency_interface_changed);
  const auto integration_head = repository_->head_sha(driver.integration_worktree());
  ASSERT_TRUE(integration_head.has_value());
  EXPECT_EQ(gates.commit_sha, *integration_head);

  // Re-integrating an already-merged milestone is the end of the
  // revalidation cycle, not a failure.
  const TurnResult again = driver.run(MilestonePhase::Integrate, "M3");
  ASSERT_TRUE(again.completed) << again.detail;
  EXPECT_TRUE(again.merged);
}

TEST_F(DriverTest, RecordsAPreTurnCheckpoint) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    write_file(std::filesystem::path(request.worktree) / "src" / "m3.cpp", "code\n");
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "done";
    return outcome;
  };
  cf::orch::StateMachine machine(*store_, "R12");
  ASSERT_TRUE(machine.register_milestone("M3").has_value());
  ASSERT_TRUE(machine.apply("M3", cf::orch::MilestoneEvent::Start, now_).has_value());
  ASSERT_TRUE(machine
                  .apply("M3", cf::orch::MilestoneEvent::TestsCommitted, now_)
                  .has_value());

  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  ASSERT_TRUE(driver.run(MilestonePhase::Code, "M3").completed);

  // The checkpoint records the worktree heads and the milestone state before
  // the turn (design section 6.12.3); recovery resets to it.
  const auto checkpoint = store_->latest_checkpoint("R12", "M3");
  ASSERT_TRUE(checkpoint.has_value()) << checkpoint.error().message;
  const auto heads = nlohmann::json::parse(checkpoint->worktree_heads_json);
  ASSERT_TRUE(heads.is_object());
  ASSERT_TRUE(heads.contains("coder"));
  const auto coder_head = repository_->head_sha(driver.worktree_for("M3", "coder"));
  ASSERT_TRUE(coder_head.has_value());
  EXPECT_NE(heads["coder"].get<std::string>(), *coder_head);  // pre-turn commit
  EXPECT_EQ(checkpoint->milestone_state, "Coding");
}

TEST_F(DriverTest, TheFixBriefCarriesTheFailingGateFindings) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    if (request.phase == MilestonePhase::Code) {
      // The build gate asserts this file does not exist (kFailingBuildProfile).
      write_file(std::filesystem::path(request.worktree) / "src" / "bad.cpp", "bad\n");
    }
    if (request.phase == MilestonePhase::Fix) {
      write_file(std::filesystem::path(request.worktree) / "src" / "fix.cpp", "fix\n");
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "done";
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kFailingBuildProfile), *repository_,
                         executor_, runner, store_.get());

  ASSERT_TRUE(driver.run(MilestonePhase::Code, "M3").completed);
  const GateOutcome gates = driver.run_gates("M3");
  EXPECT_FALSE(gates.passed);

  ASSERT_TRUE(driver.run(MilestonePhase::Fix, "M3").completed);
  ASSERT_FALSE(runner.briefs.empty());
  EXPECT_NE(runner.briefs.back().find("Failing gates from the last run"),
            std::string::npos);
  EXPECT_NE(runner.briefs.back().find("build"), std::string::npos);
}

TEST_F(DriverTest, TestRepairRunsQaAgainstTheIntegrationAndCommits) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    if (request.phase == MilestonePhase::Code) {
      write_file(std::filesystem::path(request.worktree) / "include" / "core.hpp",
                 "// core\n");
    }
    if (request.phase == MilestonePhase::Validate) {
      write_file(std::filesystem::path(request.worktree) / "tests" / "repaired_test.cpp",
                 "// repaired\n");
      AgentTurnOutcome outcome;
      outcome.completed = true;
      outcome.exit_criteria_met = true;
      outcome.summary = "tests repaired";
      return outcome;
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "done";
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  ASSERT_TRUE(driver.run(MilestonePhase::Code, "M3").completed);
  ASSERT_TRUE(driver.run(MilestonePhase::Integrate, "M3").completed);
  const auto before = repository_->head_sha(driver.integration_worktree());
  ASSERT_TRUE(before.has_value());

  const TurnResult result = driver.run_test_repair("M3");
  ASSERT_TRUE(result.completed) << result.detail;
  EXPECT_TRUE(result.exit_criteria_met);

  // The repaired tests are committed on the integration branch.
  const auto after = repository_->head_sha(driver.integration_worktree());
  ASSERT_TRUE(after.has_value());
  EXPECT_NE(*before, *after);
  const auto content =
      repository_->file_content(*after, "tests/repaired_test.cpp");
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(*content, "// repaired\n");
}

TEST_F(DriverTest, RevalidationFixStartsFromTheIntegrationHead) {
  FakeAgentRunner runner;
  runner.on_turn = [](const AgentTurnRequest& request) -> cf::Result<AgentTurnOutcome> {
    if (request.phase == MilestonePhase::Code) {
      write_file(std::filesystem::path(request.worktree) / "include" / "core.hpp",
                 "// v1\n");
    }
    if (request.phase == MilestonePhase::Fix) {
      write_file(std::filesystem::path(request.worktree) / "src" / "fix.cpp",
                 "// fixed\n");
    }
    AgentTurnOutcome outcome;
    outcome.completed = true;
    outcome.summary = "done";
    return outcome;
  };
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());

  ASSERT_TRUE(driver.run(MilestonePhase::Code, "M3").completed);
  ASSERT_TRUE(driver.run(MilestonePhase::Integrate, "M3").completed);

  // A later milestone changes the shared interface in the integration.
  const auto integration = driver.integration_worktree();
  write_file(integration / "include" / "core.hpp", "// v2\n");
  cf::git::CommitTrailers trailers;
  trailers.run_id = "R12";
  trailers.milestone_id = "M4";
  trailers.op_id = "op_m4";
  ASSERT_TRUE(repository_->commit_paths(integration, {"include/core.hpp"},
                                        "feat(M4): interface change", trailers)
                  .has_value());

  // The revalidation gates fail on the new interface and the fix must start
  // from the integration head, not the stale milestone worktree.
  driver.mark_revalidating("M3");
  const TurnResult fixed = driver.run(MilestonePhase::Fix, "M3");
  ASSERT_TRUE(fixed.completed) << fixed.detail;
  const auto coder_worktree = driver.worktree_for("M3", "coder");
  std::ifstream in(coder_worktree / "include" / "core.hpp");
  const std::string core((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(core, "// v2\n");
  EXPECT_TRUE(std::filesystem::exists(coder_worktree / "src" / "fix.cpp"));
}

TEST_F(DriverTest, WorktreeLayoutFollowsTheDesign) {
  FakeAgentRunner runner;
  WorkspaceDriver driver(plan_, config_for(kPassingProfile), *repository_, executor_,
                         runner, store_.get());
  EXPECT_EQ(driver.worktree_for("M3", "coder"),
            dir_ / "workspace" / "wt" / "R12" / "M3" / "coder");
  EXPECT_EQ(driver.integration_worktree(),
            dir_ / "workspace" / "wt" / "R12" / "integration");
}

}  // namespace
