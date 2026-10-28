// cf-tidy company checks (design section 6.5/6.7.1).
#include "cf/tidy/checks.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <yaml-cpp/yaml.h>

#include "cf/common/result.hpp"

namespace cf::tidy {
namespace {

namespace fs = std::filesystem;

constexpr const char* kNoRawNew = "cf-swt-no-raw-new";
constexpr const char* kNoRawDelete = "cf-swt-no-raw-delete";
constexpr const char* kNoRawMalloc = "cf-swt-no-raw-malloc";
constexpr const char* kNoDetachedThread = "cf-swt-no-detached-thread";
constexpr const char* kNoHardcodedSecret = "cf-swt-no-hardcoded-secret";
constexpr const char* kNoBoolParam = "cf-swt-no-bool-param";

class GuardrailCallback final
    : public clang::ast_matchers::MatchFinder::MatchCallback {
 public:
  explicit GuardrailCallback(std::vector<TidyDiagnostic>& diagnostics)
      : diagnostics_(diagnostics) {}

  void run(const clang::ast_matchers::MatchFinder::MatchResult& result) override {
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CXXNewExpr>("new")) {
      diagnose(result, expression->getBeginLoc(), kNoRawNew,
               "raw 'new' violates CG-4.2; use std::make_unique or std::make_shared");
      return;
    }
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CXXDeleteExpr>("delete")) {
      diagnose(result, expression->getBeginLoc(), kNoRawDelete,
               "raw 'delete' violates CG-4.2; use a smart pointer");
      return;
    }
    if (const auto* parameter =
            result.Nodes.getNodeAs<clang::ParmVarDecl>("bool_param")) {
      const clang::QualType type = parameter->getType();
      if (type.isNull() || !type->isBooleanType()) {
        return;
      }
      diagnose(result, parameter->getLocation(), kNoBoolParam,
               "boolean parameter violates the interface guideline; split the operation or use an enum");
      return;
    }
    if (const auto* declaration =
            result.Nodes.getNodeAs<clang::VarDecl>("secret")) {
      std::string name = declaration->getNameAsString();
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const bool looks_secret =
          name.find("password") != std::string::npos ||
          name.find("passwd") != std::string::npos ||
          name.find("secret") != std::string::npos ||
          name.find("token") != std::string::npos ||
          name.find("api_key") != std::string::npos ||
          name.find("apikey") != std::string::npos ||
          name.find("private_key") != std::string::npos;
      if (looks_secret) {
        diagnose(result, declaration->getLocation(), kNoHardcodedSecret,
                 "hardcoded credential violates the secrets guideline; read it at runtime");
      }
      return;
    }
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CallExpr>("detach")) {
      diagnose(result, expression->getBeginLoc(), kNoDetachedThread,
               "detached thread violates CG-4.2; join it or use a scope guard");
      return;
    }
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CallExpr>("alloc")) {
      // C allocation is idiomatic in C translation units; this rule applies
      // to C++ ownership, which has standard containers and smart pointers.
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus) {
        return;
      }
      const clang::FunctionDecl* callee = expression->getDirectCallee();
      if (callee == nullptr) {
        return;
      }
      const std::string_view name = callee->getName();
      if (name != "malloc" && name != "calloc" && name != "realloc" &&
          name != "free") {
        return;
      }
      diagnose(result, expression->getBeginLoc(), kNoRawMalloc,
               "raw allocation/free violates CG-4.2; use standard containers or smart pointers");
    }
  }

 private:
  void diagnose(const clang::ast_matchers::MatchFinder::MatchResult& result,
                clang::SourceLocation begin, const char* check,
                const char* message) {
    if (result.SourceManager == nullptr) {
      return;
    }
    const clang::SourceManager& source = *result.SourceManager;
    const clang::SourceLocation location = source.getExpansionLoc(begin);
    if (location.isInvalid() || source.isInSystemHeader(location)) {
      return;
    }
    const clang::PresumedLoc presumed = source.getPresumedLoc(location);
    if (presumed.isInvalid()) {
      return;
    }
    diagnostics_.push_back(TidyDiagnostic{check, message, presumed.getFilename(),
                                          presumed.getLine(),
                                          presumed.getColumn()});
  }

 private:

 private:
  std::vector<TidyDiagnostic>& diagnostics_;
};

std::optional<std::string> allowed_namespaces_from_clang_tidy(
    const fs::path& build_dir) {
  // clang-tidy reads .clang-tidy from the source tree, not the build tree.
  // Search upwards from the build directory so a target repo's standard
  // configuration also drives cf-tidy.
  std::error_code error;
  fs::path directory = fs::absolute(build_dir, error);
  if (error) {
    return std::nullopt;
  }
  for (;;) {
    const fs::path candidate = directory / ".clang-tidy";
    if (fs::is_regular_file(candidate, error)) {
      YAML::Node document;
      try {
        document = YAML::LoadFile(candidate.string());
      } catch (const YAML::Exception&) {
        return std::nullopt;
      }
      const YAML::Node options = document["CheckOptions"];
      if (!options || !options.IsSequence()) {
        return std::nullopt;
      }
      std::optional<std::string> allowed;
      for (const auto& option : options) {
        if (!option.IsMap() || !option["key"] || !option["value"]) {
          continue;
        }
        const std::string key = option["key"].as<std::string>();
        if (key.size() >= std::string("AllowedNamespaces").size() &&
            key.compare(key.size() - std::string("AllowedNamespaces").size(),
                        std::string("AllowedNamespaces").size(),
                        "AllowedNamespaces") == 0) {
          const std::string value = option["value"].as<std::string>();
          if (!value.empty()) {
            allowed = value;
          }
        }
      }
      return allowed;
    }
    const fs::path parent = directory.parent_path();
    if (parent == directory || parent.empty()) {
      break;
    }
    directory = parent;
  }
  return std::nullopt;
}

std::unique_ptr<clang::tooling::CompilationDatabase> load_database(
    const fs::path& build_dir) {
  std::string error;
  auto database = clang::tooling::CompilationDatabase::autoDetectFromDirectory(
      build_dir.string(), error);
  if (database) {
    return database;
  }
  // A profile can point at a build directory that has not been configured yet
  // when run_checks is used from a turn. Keep the check useful with a
  // conservative C++17 syntax-only fallback; the gate's build step normally
  // supplies the real compile_commands.json.
  std::vector<std::string> flags = {"-std=c++17", "-fsyntax-only"};
  return std::make_unique<clang::tooling::FixedCompilationDatabase>(
      ".", std::move(flags));
}

}  // namespace

Result<std::vector<TidyDiagnostic>> run_company_checks(
    const fs::path& build_dir, const std::vector<fs::path>& files,
    const TidyOptions& options) {
  std::vector<std::string> paths;
  paths.reserve(files.size());
  for (const auto& file : files) {
    std::error_code error;
    const fs::path absolute = fs::absolute(file, error);
    paths.push_back(error ? file.string() : absolute.string());
  }
  if (paths.empty()) {
    return std::vector<TidyDiagnostic>{};
  }

  const fs::path root = fs::absolute(build_dir);
  TidyOptions effective = options;
  if (effective.allowed_namespaces.empty()) {
    if (auto configured = allowed_namespaces_from_clang_tidy(root)) {
      effective.allowed_namespaces = std::move(*configured);
    }
  }
  auto database = load_database(root);
  clang::tooling::ClangTool tool(*database, paths);
  tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());
#ifdef CF_CLANG_RESOURCE_DIR
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      "-resource-dir=" CF_CLANG_RESOURCE_DIR,
      clang::tooling::ArgumentInsertPosition::END));
#endif

  clang::ast_matchers::MatchFinder finder;
  std::vector<TidyDiagnostic> diagnostics;
  GuardrailCallback callback(diagnostics);
  using namespace clang::ast_matchers;
  auto new_matcher = cxxNewExpr(unless(isExpansionInSystemHeader()),
                                unless(hasAnyPlacementArg(anything())));
  auto delete_matcher = cxxDeleteExpr(unless(isExpansionInSystemHeader()));
  auto alloc_matcher = callExpr(unless(isExpansionInSystemHeader()));
  auto detach_matcher = cxxMemberCallExpr(
      unless(isExpansionInSystemHeader()),
      callee(cxxMethodDecl(hasName("detach"))));
  auto secret_matcher = varDecl(
      unless(isExpansionInSystemHeader()),
      hasInitializer(ignoringImplicit(stringLiteral())));
  auto bool_param_matcher = parmVarDecl(unless(isExpansionInSystemHeader()));
  if (!effective.allowed_namespaces.empty()) {
    const auto allowed = hasAncestor(
        namespaceDecl(matchesName(effective.allowed_namespaces)));
    new_matcher = cxxNewExpr(unless(isExpansionInSystemHeader()),
                             unless(hasAnyPlacementArg(anything())),
                             unless(allowed));
    delete_matcher =
        cxxDeleteExpr(unless(isExpansionInSystemHeader()), unless(allowed));
    alloc_matcher =
        callExpr(unless(isExpansionInSystemHeader()), unless(allowed));
    detach_matcher = cxxMemberCallExpr(
        unless(isExpansionInSystemHeader()), unless(allowed),
        callee(cxxMethodDecl(hasName("detach"))));
    secret_matcher = varDecl(
        unless(isExpansionInSystemHeader()), hasInitializer(ignoringImplicit(stringLiteral())));
  }
  finder.addMatcher(std::move(new_matcher).bind("new"), &callback);
  finder.addMatcher(std::move(delete_matcher).bind("delete"), &callback);
  finder.addMatcher(std::move(alloc_matcher).bind("alloc"), &callback);
  finder.addMatcher(std::move(detach_matcher).bind("detach"), &callback);
  finder.addMatcher(std::move(secret_matcher).bind("secret"), &callback);
  finder.addMatcher(std::move(bool_param_matcher).bind("bool_param"), &callback);

  auto factory = clang::tooling::newFrontendActionFactory(&finder);
  const int result = tool.run(factory.get());
  if (result != 0) {
    return fail("TIDY_PARSE_FAILED",
                "cf-tidy could not parse one or more translation units");
  }

  std::sort(diagnostics.begin(), diagnostics.end(),
            [](const TidyDiagnostic& left, const TidyDiagnostic& right) {
              if (left.file != right.file) {
                return left.file < right.file;
              }
              if (left.line != right.line) {
                return left.line < right.line;
              }
              if (left.column != right.column) {
                return left.column < right.column;
              }
              return left.check < right.check;
            });
  diagnostics.erase(
      std::unique(diagnostics.begin(), diagnostics.end(),
                  [](const TidyDiagnostic& left, const TidyDiagnostic& right) {
                    return left.file == right.file && left.line == right.line &&
                           left.column == right.column &&
                           left.check == right.check;
                  }),
      diagnostics.end());
  return diagnostics;
}

}  // namespace cf::tidy

namespace cf::tidy {

std::vector<CheckInfo> implemented_checks() {
  return {
      {"cf-swt-no-raw-new", "CG-4.2",
       "raw new in project code; use smart pointers"},
      {"cf-swt-no-raw-delete", "CG-4.2",
       "raw delete in project code; use smart pointers"},
      {"cf-swt-no-raw-malloc", "CG-4.2",
       "raw malloc/calloc/realloc/free in C++; use containers or smart pointers"},
      {"cf-swt-no-detached-thread", "CG-4.2",
       "detached threads; join the thread or use a scope guard"},
      {"cf-swt-no-hardcoded-secret", "CG-SECRETS",
       "credential-like variables initialized with a string literal"},
      {"cf-swt-no-bool-param", "CG-INTERFACES",
       "boolean parameters; split the operation or use an enum"},
  };
}

}  // namespace cf::tidy
