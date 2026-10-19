#include "cf/eval/hidden.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace cf::eval {
namespace {

using Json = nlohmann::json;

Result<HiddenStep> parse_step(const YAML::Node& node, std::size_t index) {
  HiddenStep step;
  YAML::Node argv_node = node;
  if (node.IsMap()) {
    step.name = node["name"].as<std::string>("");
    argv_node = node["argv"];
    if (!argv_node || !argv_node.IsSequence()) {
      return fail(error_codes::kHiddenParseFailed,
                  "step " + std::to_string(index) + " needs an 'argv' sequence");
    }
  }
  if (!argv_node.IsSequence()) {
    return fail(error_codes::kHiddenParseFailed,
                "step " + std::to_string(index) + " must be an argv sequence");
  }
  for (const auto& element : argv_node) {
    step.argv.push_back(element.as<std::string>(""));
  }
  if (step.argv.empty()) {
    return fail(error_codes::kHiddenParseFailed,
                "step " + std::to_string(index) + " has an empty argv");
  }
  return step;
}

Result<std::vector<HiddenStep>> parse_steps(const YAML::Node& node,
                                            const std::string& section) {
  std::vector<HiddenStep> steps;
  if (!node) {
    return steps;
  }
  if (!node.IsSequence()) {
    return fail(error_codes::kHiddenParseFailed,
                "'" + section + "' must be a sequence");
  }
  std::size_t index = 0;
  for (const auto& step : node) {
    auto parsed = parse_step(step, index++);
    if (!parsed.has_value()) {
      return tl::make_unexpected(parsed.error());
    }
    steps.push_back(std::move(*parsed));
  }
  return steps;
}

}  // namespace

Result<HiddenTestSpec> parse_hidden_test_spec(std::string_view yaml,
                                              const std::filesystem::path& base_dir) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(yaml));
  } catch (const YAML::Exception& error) {
    return fail(error_codes::kHiddenParseFailed,
                std::string("hidden test yaml: ") + error.what());
  }
  if (!root.IsMap()) {
    return fail(error_codes::kHiddenParseFailed, "the spec must be a mapping");
  }

  HiddenTestSpec spec;
  spec.base_dir = base_dir;
  spec.security = root["security"].as<bool>(false);
  spec.recovery = root["recovery"].as<bool>(false);
  spec.crash_after_milestone = root["crash_after_milestone"].as<std::string>("");
  spec.crash_after_seconds = root["crash_after_seconds"].as<int>(0);
  spec.crash_count = root["crash_count"].as<int>(1);
  if (spec.crash_after_seconds < 0) {
    return fail("EVAL_HIDDEN_INVALID", "crash_after_seconds cannot be negative");
  }
  if (spec.crash_count < 1 || spec.crash_count > 5) {
    return fail("EVAL_HIDDEN_INVALID", "crash_count must be between 1 and 5");
  }
  if (spec.crash_count > 1 && spec.crash_after_seconds <= 0) {
    return fail("EVAL_HIDDEN_INVALID",
                "crash_count > 1 needs crash_after_seconds for the later kills");
  }

  auto compile = parse_steps(root["compile"], "compile");
  if (!compile.has_value()) {
    return tl::make_unexpected(compile.error());
  }
  spec.compile = std::move(*compile);
  auto tests = parse_steps(root["tests"], "tests");
  if (!tests.has_value()) {
    return tl::make_unexpected(tests.error());
  }
  spec.tests = std::move(*tests);

  if (spec.tests.empty()) {
    return fail(error_codes::kHiddenInvalid, "the spec has no test steps");
  }
  return spec;
}

Result<HiddenTestSpec> load_hidden_test_spec(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(error_codes::kHiddenParseFailed,
                "cannot read hidden test spec: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return parse_hidden_test_spec(buffer.str(), path.parent_path());
}

std::vector<std::string> expand_hidden_argv(const std::vector<std::string>& argv,
                                            const std::filesystem::path& worktree,
                                            const std::filesystem::path& hidden_dir,
                                            const std::filesystem::path& build_dir,
                                            const std::filesystem::path& run_dir) {
  std::vector<std::string> expanded;
  expanded.reserve(argv.size());
  for (const auto& element : argv) {
    std::string value = element;
    const auto replace_all = [&value](const std::string& needle,
                                      const std::string& replacement) {
      std::size_t position = 0;
      while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
      }
    };
    replace_all("{worktree}", worktree.string());
    replace_all("{hidden}", hidden_dir.string());
    replace_all("{build}", build_dir.string());
    replace_all("{run_dir}", run_dir.string());
    expanded.push_back(std::move(value));
  }
  return expanded;
}

bool parse_test_counts(std::string_view output, TestCounts* counts) {
  std::istringstream stream{std::string(output)};
  std::string line;
  TestCounts found;
  bool has_counts = false;
  while (std::getline(stream, line)) {
    if (line.empty() || line.front() != '{') {
      continue;
    }
    Json parsed = Json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
      continue;
    }
    if (!parsed.contains("tests") || !parsed.contains("passed")) {
      continue;
    }
    if (!parsed["tests"].is_number_integer() ||
        !parsed["passed"].is_number_integer()) {
      continue;
    }
    found.tests = parsed["tests"].get<int>();
    found.passed = parsed["passed"].get<int>();
    has_counts = true;
  }
  if (has_counts && counts != nullptr) {
    *counts = found;
  }
  return has_counts;
}

HiddenTestResult run_hidden_tests(const HiddenTestSpec& spec,
                                  const std::filesystem::path& worktree,
                                  const std::filesystem::path& build_dir,
                                  process::ProcessExecutor& executor,
                                  std::chrono::milliseconds timeout,
                                  const std::filesystem::path& run_dir) {
  HiddenTestResult result;
  std::error_code ec;
  std::filesystem::create_directories(build_dir, ec);
  if (ec) {
    result.ok = false;
    result.error = "cannot create the hidden test build directory: " + ec.message();
    return result;
  }

  for (const auto& step : spec.compile) {
    process::ProcessRequest request;
    request.argv =
        expand_hidden_argv(step.argv, worktree, spec.base_dir, build_dir, run_dir);
    request.working_directory = worktree.string();
    request.timeout = timeout;
    const process::ProcessResult run = executor.run(request);
    if (!run.launched || run.exit_code != 0) {
      result.ok = false;
      result.error = "compile step '" +
                     (step.name.empty() ? request.argv.front() : step.name) +
                     "' failed";
      result.log.push_back(result.error);
      // Every test counts as failed; the total is unknown without running.
      result.tests = static_cast<int>(spec.tests.size());
      result.passed = 0;
      return result;
    }
    result.log.push_back("compile '" +
                         (step.name.empty() ? request.argv.front() : step.name) +
                         "' ok");
  }

  for (const auto& step : spec.tests) {
    process::ProcessRequest request;
    request.argv =
        expand_hidden_argv(step.argv, worktree, spec.base_dir, build_dir, run_dir);
    request.working_directory = worktree.string();
    request.timeout = timeout;
    const process::ProcessResult run = executor.run(request);
    const std::string label =
        step.name.empty() ? request.argv.front() : step.name;
    TestCounts counts;
    if (parse_test_counts(run.stdout_text, &counts)) {
      result.tests += counts.tests;
      result.passed += counts.passed;
      result.log.push_back("test '" + label + "': " + std::to_string(counts.passed) +
                           "/" + std::to_string(counts.tests) + " passed");
    } else {
      ++result.tests;
      if (run.launched && run.exit_code == 0) {
        ++result.passed;
        result.log.push_back("test '" + label + "' passed");
      } else {
        result.log.push_back("test '" + label + "' failed (exit " +
                             std::to_string(run.exit_code) + ")");
      }
    }
  }
  result.ok = result.passed == result.tests;
  return result;
}

}  // namespace cf::eval
