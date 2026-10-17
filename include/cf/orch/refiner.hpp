// cf-orch: the plan refiner (design section 6.2, Phase 4).
//
// Turns an approved implementation plan into a design package: a component
// breakdown with a refined milestone DAG (writes/uses/interfaces for the
// parallel-safety rules of section 6.13.2) and one ADR per design decision.
// The planner model is called directly through cf-llm (agent turns go through
// opencode); the DesignModel interface keeps tests offline.
//
// Pipeline (design section 6.2): design turn, self-critique by a second
// persona, one revision, then the human approval gate (AwaitingDesignApproval)
// and ingestion into cf-rag once approved.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
#include "cf/plan/plan.hpp"

namespace cf::rag {
class Retriever;
}

namespace cf::orch {

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kRefineFailed = "ORCH_REFINE_FAILED";
inline constexpr const char* kDesignInvalid = "ORCH_DESIGN_INVALID";
}  // namespace error_codes

// One architecture decision record (design section 6.2, step 3).
struct DesignAdr {
  std::string id;  // e.g. ADR-001
  std::string title;
  std::string decision;
  std::vector<std::string> alternatives;
};

// One milestone of the refined DAG.
struct DesignMilestone {
  std::string id;
  std::string title;
  std::vector<std::string> requirements;
  std::vector<std::string> depends_on;
  std::vector<std::string> acceptance;
  std::vector<std::string> writes;
  std::vector<std::string> uses;
  std::vector<std::string> writes_interfaces;
};

// The design package (stored as runs/<run>/design.json).
struct DesignPackage {
  std::string plan_id;
  std::string product;
  std::string model;  // the planner model id, for the record
  std::string system_design;
  std::vector<DesignMilestone> milestones;
  std::vector<DesignAdr> adrs;
};

// The planner model. Production wraps llm::FireworksClient::chat; tests
// script the responses.
class DesignModel {
 public:
  virtual ~DesignModel() = default;
  // Returns the raw assistant text for one prompt.
  virtual Result<std::string> complete(std::string_view prompt) = 0;
};

struct DesignRefinerOptions {
  std::string model;              // recorded in the package
  std::size_t context_hits = 6;   // retrieved documents included in the prompt
  bool self_critique = true;      // design section 6.2, step 5
};

class DesignRefiner {
 public:
  DesignRefiner(DesignModel& model, const rag::Retriever* docs = nullptr,
                DesignRefinerOptions options = {});

  // Runs the pipeline: design, self-critique, at most one revision. The
  // returned package is validated against the plan.
  Result<DesignPackage> refine(const plan::Plan& plan);

 private:
  std::string design_prompt(const plan::Plan& plan) const;
  std::string critique_prompt(const plan::Plan& plan,
                              const DesignPackage& package) const;
  std::string revision_prompt(const plan::Plan& plan, const DesignPackage& package,
                              const std::vector<std::string>& issues) const;

  DesignModel& model_;
  const rag::Retriever* docs_ = nullptr;
  DesignRefinerOptions options_;
};

// Parses a design package from model output; code fences and prose around the
// JSON object are tolerated.
Result<DesignPackage> parse_design_package(std::string_view text);

// Checks the package against the plan (design section 6.2): milestone IDs and
// requirement mappings are fixed, dependencies may only be added, the DAG is
// acyclic, acceptance criteria and ADRs are present.
Status validate_design_package(const DesignPackage& package, const plan::Plan& plan);

// Builds the refined plan (same target, inference and budget; milestones from
// the package) that drives the scheduler's parallel-safety rules.
plan::Plan design_package_to_plan(const DesignPackage& package,
                                  const plan::Plan& original);

std::string design_package_to_json(const DesignPackage& package);
// Human-readable summary for the approval gate.
std::string design_package_to_markdown(const DesignPackage& package);

// Publishes an approved design package as a RAG design document under
// <workspace>/designs (design section 6.2 step 7): front-matter Markdown
// with scope=product, status=approved, the source run and the approval date.
// A design previously approved for the same plan becomes status=superseded
// and is no longer returned by the retriever. Returns the written path.
Result<std::filesystem::path> publish_design_package(
    const std::filesystem::path& workspace, const std::string& run_id,
    const DesignPackage& package, int64_t approved_at);

}  // namespace cf::orch
