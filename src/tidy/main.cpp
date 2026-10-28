// cf-tidy: run the company C/C++ guardrail checks (design section 6.7.1).
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "cf/tidy/checks.hpp"

namespace {

int print_coverage(const std::string& path) {
  try {
    const YAML::Node document = YAML::LoadFile(path);
    std::size_t automated = 0;
    std::size_t reviewer_only = 0;
    for (const auto& rule : document["rules"]) {
      const std::string status = rule["status"].as<std::string>();
      if (status == "automated") {
        ++automated;
      } else if (status == "reviewer-only") {
        ++reviewer_only;
      }
    }
    std::cout << "automated rules: " << automated << "\n"
              << "reviewer-only rules: " << reviewer_only << "\n"
              << "compiled checks: " << cf::tidy::implemented_checks().size()
              << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cf-tidy: cannot read rule catalog: " << error.what() << "\n";
    return 2;
  }
}

void print_usage() {
  std::cout << "usage: cf-tidy -p <build_dir> [--allowed-namespaces=<regex>] "
               "<changed files...>\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path build_dir;
  std::vector<std::filesystem::path> files;
  cf::tidy::TidyOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      print_usage();
      return 0;
    }
    if (argument == "--list-checks") {
      for (const auto& check : cf::tidy::implemented_checks()) {
        std::cout << check.id << "  " << check.rule << "  " << check.description
                  << "\n";
      }
      return 0;
    }
    if (argument == "-p" || argument == "--build-path") {
      if (i + 1 >= argc) {
        print_usage();
        return 2;
      }
      build_dir = argv[++i];
      continue;
    }
    const std::string build_prefix = "--build-path=";
    if (argument.rfind(build_prefix, 0) == 0) {
      build_dir = argument.substr(build_prefix.size());
      continue;
    }
    const std::string namespaces_prefix = "--allowed-namespaces=";
    if (argument.rfind(namespaces_prefix, 0) == 0) {
      options.allowed_namespaces = argument.substr(namespaces_prefix.size());
      continue;
    }
    const std::string disable_prefix = "--disable-check=";
    if (argument.rfind(disable_prefix, 0) == 0) {
      options.disabled_checks.push_back(argument.substr(disable_prefix.size()));
      continue;
    }
    const std::string coverage_prefix = "--coverage=";
    if (argument.rfind(coverage_prefix, 0) == 0) {
      return print_coverage(argument.substr(coverage_prefix.size()));
    }
    const std::string only_prefix = "--only-check=";
    if (argument.rfind(only_prefix, 0) == 0) {
      options.only_checks.push_back(argument.substr(only_prefix.size()));
      continue;
    }
    if (!argument.empty() && argument[0] == '-') {
      std::cerr << "cf-tidy: unknown option '" << argument << "'\n";
      print_usage();
      return 2;
    }
    files.emplace_back(argument);
  }
  if (build_dir.empty() || files.empty()) {
    print_usage();
    return 2;
  }

  auto diagnostics = cf::tidy::run_company_checks(build_dir, files, options);
  if (!diagnostics) {
    std::cerr << "cf-tidy: " << diagnostics.error().message << "\n";
    return 2;
  }
  for (const auto& diagnostic : *diagnostics) {
    std::cerr << diagnostic.file << ":" << diagnostic.line << ":"
              << diagnostic.column << ": warning: " << diagnostic.message
              << " [" << diagnostic.check << "]\n";
  }
  return diagnostics->empty() ? 0 : 1;
}
