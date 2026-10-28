// cf-tidy: run the company C/C++ guardrail checks (design section 6.7.1).
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "cf/tidy/checks.hpp"

namespace {

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
