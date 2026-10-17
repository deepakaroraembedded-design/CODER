#include "cf/orch/refiner.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include <spdlog/spdlog.h>

#include "cf/rag/retriever.hpp"

namespace cf::orch {
namespace {

using Json = nlohmann::json;

std::string join(const std::vector<std::string>& values,
                 std::string_view separator = ", ") {
  std::string out;
  for (const auto& value : values) {
    if (!out.empty()) {
      out += separator;
    }
    out += value;
  }
  return out;
}

std::vector<std::string> string_list(const Json& node) {
  std::vector<std::string> out;
  if (!node.is_array()) {
    return out;
  }
  for (const auto& item : node) {
    if (item.is_string()) {
      out.push_back(item.get<std::string>());
    }
  }
  return out;
}

Json milestone_to_json(const DesignMilestone& milestone) {
  return Json{{"id", milestone.id},
              {"title", milestone.title},
              {"requirements", milestone.requirements},
              {"depends_on", milestone.depends_on},
              {"acceptance", milestone.acceptance},
              {"writes", milestone.writes},
              {"uses", milestone.uses},
              {"writes_interfaces", milestone.writes_interfaces}};
}

DesignMilestone milestone_from_json(const Json& node) {
  DesignMilestone milestone;
  milestone.id = node.value("id", "");
  milestone.title = node.value("title", "");
  milestone.requirements = string_list(node.value("requirements", Json::array()));
  milestone.depends_on = string_list(node.value("depends_on", Json::array()));
  milestone.acceptance = string_list(node.value("acceptance", Json::array()));
  milestone.writes = string_list(node.value("writes", Json::array()));
  milestone.uses = string_list(node.value("uses", Json::array()));
  milestone.writes_interfaces =
      string_list(node.value("writes_interfaces", Json::array()));
  return milestone;
}

Json package_to_json(const DesignPackage& package) {
  Json milestones = Json::array();
  for (const auto& milestone : package.milestones) {
    milestones.push_back(milestone_to_json(milestone));
  }
  Json adrs = Json::array();
  for (const auto& adr : package.adrs) {
    adrs.push_back(Json{{"id", adr.id},
                        {"title", adr.title},
                        {"decision", adr.decision},
                        {"alternatives", adr.alternatives}});
  }
  return Json{{"plan_id", package.plan_id},
              {"product", package.product},
              {"model", package.model},
              {"system_design", package.system_design},
              {"milestones", std::move(milestones)},
              {"adrs", std::move(adrs)}};
}

std::string plan_summary(const plan::Plan& plan) {
  std::ostringstream out;
  out << "plan_id: " << plan.plan_id << "\n";
  out << "product: " << plan.product << "\n";
  out << "version: " << plan.version << "\n";
  if (!plan.requirements_ref.empty()) {
    out << "requirements_ref: " << plan.requirements_ref << "\n";
  }
  out << "target: " << plan.target.language << " " << plan.target.standard << " ("
      << plan.target.build_system << ", " << plan.target.test_framework << ")\n";
  if (!plan.forbidden.empty()) {
    out << "forbidden: " << join(plan.forbidden, "; ") << "\n";
  }
  out << "milestones:\n";
  for (const auto& milestone : plan.milestones) {
    out << "  - id: " << milestone.id << "\n";
    out << "    title: " << milestone.title << "\n";
    out << "    requirements: [" << join(milestone.requirements) << "]\n";
    out << "    depends_on: [" << join(milestone.depends_on) << "]\n";
    for (const auto& criterion : milestone.acceptance) {
      out << "    acceptance: " << criterion << "\n";
    }
  }
  return out.str();
}

const char* kDesignRules = R"rules(
Rules:
- Keep every milestone id exactly as in the plan, and keep each milestone's
  requirement mapping exactly (same requirement ids).
- You may add dependencies between milestones, never remove one declared in
  the plan.
- `writes` are repo-relative paths or components the milestone changes;
  `uses` and `writes_interfaces` are public interface paths or ids. They must
  be complete enough that independent milestones can run in parallel without
  touching the same files or interfaces (design section 6.13.2).
- Give every milestone concrete entry/exit acceptance criteria.
- Record at least one ADR per component-level pattern or interface choice.
- Respect the plan's forbidden constraints and the retrieved company
  guidelines; cite the guideline doc ids in the ADRs where you follow them.
)rules";

std::string retrieved_context(const rag::Retriever* docs, const plan::Plan& plan,
                              std::size_t top_k) {
  if (docs == nullptr || top_k == 0) {
    return {};
  }
  std::string query_text = plan.product;
  for (const auto& milestone : plan.milestones) {
    query_text += " " + milestone.title;
  }
  rag::Query query;
  query.text = query_text;
  query.top_k = static_cast<uint32_t>(top_k);
  query.context.product = plan.product;
  query.context.language = plan.target.language;
  auto hits = docs->search(query);
  if (!hits) {
    spdlog::warn("refiner: document search failed: {}", hits.error().message);
    return {};
  }
  std::ostringstream out;
  for (const auto& hit : *hits) {
    out << "[" << hit.doc_id << "@" << hit.version << "] "
        << (hit.citation.empty() ? std::string("no citation") : hit.citation) << "\n";
    out << hit.text << "\n\n";
  }
  return out.str();
}

}  // namespace

Result<DesignPackage> parse_design_package(std::string_view text) {
  const auto begin = text.find('{');
  const auto end = text.rfind('}');
  if (begin == std::string_view::npos || end == std::string_view::npos || end < begin) {
    return fail(error_codes::kDesignInvalid,
                "the planner output contains no JSON object");
  }
  Json node = Json::parse(text.substr(begin, end - begin + 1), nullptr, false);
  if (node.is_discarded() || !node.is_object()) {
    return fail(error_codes::kDesignInvalid, "the planner output is not valid JSON");
  }
  DesignPackage package;
  package.plan_id = node.value("plan_id", "");
  package.product = node.value("product", "");
  package.model = node.value("model", "");
  package.system_design = node.value("system_design", "");
  for (const auto& item : node.value("milestones", Json::array())) {
    package.milestones.push_back(milestone_from_json(item));
  }
  for (const auto& item : node.value("adrs", Json::array())) {
    DesignAdr adr;
    adr.id = item.value("id", "");
    adr.title = item.value("title", "");
    adr.decision = item.value("decision", "");
    adr.alternatives = string_list(item.value("alternatives", Json::array()));
    package.adrs.push_back(std::move(adr));
  }
  return package;
}

Status validate_design_package(const DesignPackage& package, const plan::Plan& plan) {
  if (package.system_design.empty()) {
    return fail(error_codes::kDesignInvalid, "the design has no system_design");
  }
  if (package.milestones.empty()) {
    return fail(error_codes::kDesignInvalid, "the design has no milestones");
  }

  std::set<std::string> plan_ids;
  for (const auto& milestone : plan.milestones) {
    plan_ids.insert(milestone.id);
  }
  std::set<std::string> design_ids;
  for (const auto& milestone : package.milestones) {
    if (!design_ids.insert(milestone.id).second) {
      return fail(error_codes::kDesignInvalid,
                  "duplicate milestone id in the design: " + milestone.id);
    }
  }
  if (design_ids != plan_ids) {
    return fail(error_codes::kDesignInvalid,
                "the design must keep exactly the plan's milestone ids");
  }

  for (const auto& milestone : package.milestones) {
    const plan::MilestoneSpec* original = plan.find_milestone(milestone.id);
    if (original == nullptr) {
      return fail(error_codes::kDesignInvalid,
                  "unknown milestone " + milestone.id);
    }
    std::set<std::string> expected(original->requirements.begin(),
                                   original->requirements.end());
    std::set<std::string> actual(milestone.requirements.begin(),
                                 milestone.requirements.end());
    if (expected != actual) {
      return fail(error_codes::kDesignInvalid,
                  "milestone " + milestone.id +
                      " must keep exactly its plan requirements");
    }
    for (const auto& dependency : original->depends_on) {
      if (std::find(milestone.depends_on.begin(), milestone.depends_on.end(),
                    dependency) == milestone.depends_on.end()) {
        return fail(error_codes::kDesignInvalid,
                    "milestone " + milestone.id + " dropped the plan dependency " +
                        dependency);
      }
    }
    for (const auto& dependency : milestone.depends_on) {
      if (plan_ids.count(dependency) == 0) {
        return fail(error_codes::kDesignInvalid,
                    "milestone " + milestone.id + " depends on unknown milestone " +
                        dependency);
      }
    }
    if (milestone.acceptance.empty()) {
      return fail(error_codes::kDesignInvalid,
                  "milestone " + milestone.id + " has no acceptance criteria");
    }
  }

  // The refined graph must stay acyclic (Kahn).
  std::map<std::string, std::size_t> pending;
  std::map<std::string, std::vector<std::string>, std::less<>> dependents;
  for (const auto& milestone : package.milestones) {
    pending[milestone.id] = milestone.depends_on.size();
    for (const auto& dependency : milestone.depends_on) {
      dependents[dependency].push_back(milestone.id);
    }
  }
  std::vector<std::string> ready;
  for (const auto& [id, count] : pending) {
    if (count == 0) {
      ready.push_back(id);
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const std::string id = ready.back();
    ready.pop_back();
    ++visited;
    for (const auto& dependent : dependents[id]) {
      if (--pending[dependent] == 0) {
        ready.push_back(dependent);
      }
    }
  }
  if (visited != package.milestones.size()) {
    return fail(error_codes::kDesignInvalid, "the design milestone graph has a cycle");
  }

  if (package.adrs.empty()) {
    return fail(error_codes::kDesignInvalid, "the design has no ADRs");
  }
  std::set<std::string> adr_ids;
  for (const auto& adr : package.adrs) {
    if (adr.id.empty() || adr.title.empty() || adr.decision.empty()) {
      return fail(error_codes::kDesignInvalid,
                  "every ADR needs an id, a title and a decision");
    }
    if (!adr_ids.insert(adr.id).second) {
      return fail(error_codes::kDesignInvalid, "duplicate ADR id " + adr.id);
    }
  }
  return {};
}

plan::Plan design_package_to_plan(const DesignPackage& package,
                                  const plan::Plan& original) {
  plan::Plan refined = original;
  refined.milestones.clear();
  for (const auto& milestone : package.milestones) {
    plan::MilestoneSpec spec;
    spec.id = milestone.id;
    spec.title = milestone.title;
    spec.requirements = milestone.requirements;
    spec.depends_on = milestone.depends_on;
    spec.acceptance = milestone.acceptance;
    spec.writes = milestone.writes;
    spec.uses = milestone.uses;
    spec.writes_interfaces = milestone.writes_interfaces;
    refined.milestones.push_back(std::move(spec));
  }
  return refined;
}

std::string design_package_to_json(const DesignPackage& package) {
  return package_to_json(package).dump(2) + "\n";
}

std::string design_package_to_markdown(const DesignPackage& package) {
  std::ostringstream out;
  out << "# Design package: " << package.plan_id << "\n\n";
  out << "Product: " << package.product << "  \n";
  out << "Planner model: " << package.model << "\n\n";
  out << "## System design\n\n" << package.system_design << "\n\n";
  out << "## Milestones\n\n";
  for (const auto& milestone : package.milestones) {
    out << "### " << milestone.id << " — " << milestone.title << "\n\n";
    out << "- Requirements: " << join(milestone.requirements) << "\n";
    out << "- Depends on: "
        << (milestone.depends_on.empty() ? std::string("none")
                                         : join(milestone.depends_on))
        << "\n";
    out << "- Writes: "
        << (milestone.writes.empty() ? std::string("none") : join(milestone.writes))
        << "\n";
    out << "- Uses: "
        << (milestone.uses.empty() ? std::string("none") : join(milestone.uses))
        << "\n";
    out << "- Writes interfaces: "
        << (milestone.writes_interfaces.empty() ? std::string("none")
                                                : join(milestone.writes_interfaces))
        << "\n";
    out << "- Acceptance criteria:\n";
    for (const auto& criterion : milestone.acceptance) {
      out << "  - " << criterion << "\n";
    }
    out << "\n";
  }
  out << "## Architecture decision records\n\n";
  for (const auto& adr : package.adrs) {
    out << "### " << adr.id << " — " << adr.title << "\n\n";
    out << adr.decision << "\n\n";
    if (!adr.alternatives.empty()) {
      out << "Alternatives considered: " << join(adr.alternatives, "; ") << "\n\n";
    }
  }
  return out.str();
}

DesignRefiner::DesignRefiner(DesignModel& model, const rag::Retriever* docs,
                             DesignRefinerOptions options)
    : model_(model), docs_(docs), options_(std::move(options)) {}

std::string DesignRefiner::design_prompt(const plan::Plan& plan) const {
  std::ostringstream out;
  out << "You are the planner agent of the Coder Framework (design section 6.2). "
         "Produce the component-level design for this implementation plan.\n\n"
      << "Plan:\n"
      << plan_summary(plan) << "\n";
  const std::string context = retrieved_context(docs_, plan, options_.context_hits);
  if (!context.empty()) {
    out << "Company documents and reference material:\n" << context << "\n";
  }
  out << kDesignRules
      << "\nReturn exactly one JSON object, with no prose around it:\n"
         "{\n"
         "  \"system_design\": \"component breakdown, layering, cross-cutting "
         "concerns (error handling, logging, concurrency, memory ownership) and "
         "system-level pattern choices\",\n"
         "  \"milestones\": [\n"
         "    {\"id\": \"<plan milestone id>\", \"title\": \"...\",\n"
         "     \"requirements\": [\"<exactly the plan's ids for this milestone>\"],\n"
         "     \"depends_on\": [\"<plan dependencies plus any you add>\"],\n"
         "     \"acceptance\": [\"<entry and exit criteria>\"],\n"
         "     \"writes\": [\"<repo-relative paths or components>\"],\n"
         "     \"uses\": [\"<public interfaces used>\"],\n"
         "     \"writes_interfaces\": [\"<public interfaces changed>\"]}\n"
         "  ],\n"
         "  \"adrs\": [\n"
         "    {\"id\": \"ADR-001\", \"title\": \"...\", \"decision\": \"...\",\n"
         "     \"alternatives\": [\"...\"]}\n"
         "  ]\n"
         "}\n";
  return out.str();
}

std::string DesignRefiner::critique_prompt(const plan::Plan& plan,
                                           const DesignPackage& package) const {
  std::ostringstream out;
  out << "You are the reviewer persona of the Coder Framework. Critique this "
         "design package against the plan and the guardrails. Return exactly one "
         "JSON object: {\"verdict\": \"ok\" or \"revise\", \"issues\": [\"...\"]}. "
         "Use \"revise\" only for problems that matter: missing requirement "
         "coverage, unsafe parallel writes, missing or wrong interfaces, "
         "contradictory decisions, or violations of the plan's forbidden "
         "constraints.\n\n"
      << "Plan:\n"
      << plan_summary(plan) << "\n"
      << "Design package:\n"
      << package_to_json(package).dump() << "\n";
  return out.str();
}

std::string DesignRefiner::revision_prompt(
    const plan::Plan& plan, const DesignPackage& package,
    const std::vector<std::string>& issues) const {
  std::ostringstream out;
  out << "Revise this design package to resolve the review issues. Return the "
         "full JSON object again, in the same shape.\n\n"
      << "Issues:\n";
  for (const auto& issue : issues) {
    out << "- " << issue << "\n";
  }
  out << "\nPlan:\n" << plan_summary(plan) << "\n";
  out << kDesignRules;
  out << "\nDesign package:\n" << package_to_json(package).dump() << "\n";
  return out.str();
}

Result<DesignPackage> DesignRefiner::refine(const plan::Plan& plan) {
  auto first = model_.complete(design_prompt(plan));
  if (!first) {
    return fail(error_codes::kRefineFailed,
                "the planner call failed: " + first.error().message);
  }
  auto package = parse_design_package(*first);
  if (!package) {
    return package;
  }
  package->plan_id = plan.plan_id;
  package->product = plan.product;
  package->model = options_.model;
  auto valid = validate_design_package(*package, plan);
  if (!valid) {
    return tl::make_unexpected(valid.error());
  }
  if (!options_.self_critique) {
    return package;
  }

  // Self-critique (design section 6.2, step 5). A critique that cannot be
  // read is not fatal: the valid design stands and the revision is skipped.
  auto critique_text = model_.complete(critique_prompt(plan, *package));
  if (!critique_text) {
    spdlog::warn("refiner: self-critique call failed: {}",
                 critique_text.error().message);
    return package;
  }
  const auto begin = critique_text->find('{');
  const auto end = critique_text->rfind('}');
  const Json critique =
      begin == std::string::npos || end == std::string::npos
          ? Json()
          : Json::parse(critique_text->substr(begin, end - begin + 1), nullptr, false);
  if (!critique.is_object()) {
    const std::string preview = critique_text->substr(0, 160);
    spdlog::warn("refiner: self-critique returned no usable JSON ({}); keeping "
                 "the design",
                 preview);
    return package;
  }
  const std::vector<std::string> issues =
      string_list(critique.value("issues", Json::array()));
  if (critique.value("verdict", "") != "revise" || issues.empty()) {
    return package;
  }

  auto revised_text = model_.complete(revision_prompt(plan, *package, issues));
  if (!revised_text) {
    spdlog::warn("refiner: revision call failed: {}", revised_text.error().message);
    return package;
  }
  auto revised = parse_design_package(*revised_text);
  if (!revised) {
    spdlog::warn("refiner: revision is not usable ({}); keeping the first design",
                 revised.error().message);
    return package;
  }
  revised->plan_id = plan.plan_id;
  revised->product = plan.product;
  revised->model = options_.model;
  valid = validate_design_package(*revised, plan);
  if (!valid) {
    spdlog::warn("refiner: revision is invalid ({}); keeping the first design",
                 valid.error().message);
    return package;
  }
  return revised;
}

}  // namespace cf::orch
