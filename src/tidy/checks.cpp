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
constexpr const char* kNoCStyleCast = "cf-swt-no-c-style-cast";
constexpr const char* kNoUsingNamespaceInHeader = "cf-swt-no-using-namespace-in-header";
constexpr const char* kNoGoto = "cf-swt-no-goto";
constexpr const char* kNoReinterpretCast = "cf-swt-no-reinterpret-cast";
constexpr const char* kNoConstCast = "cf-swt-no-const-cast";
constexpr const char* kNoRawArray = "cf-swt-no-raw-array";
constexpr const char* kNoUnsafeCFunction = "cf-swt-no-unsafe-c-function";
constexpr const char* kNoMultipleInheritance = "cf-swt-no-multiple-inheritance";
constexpr const char* kNoPublicDataMember = "cf-swt-no-public-data-member";
constexpr const char* kNoSleepInTest = "cf-swt-no-sleep-in-test";
constexpr const char* kNoPublicStaticData = "cf-swt-no-public-static-data";
constexpr const char* kNoWeakHash = "cf-swt-no-weak-hash";

class GuardrailCallback final
    : public clang::ast_matchers::MatchFinder::MatchCallback {
 public:
  GuardrailCallback(std::vector<TidyDiagnostic>& diagnostics,
                    const TidyOptions& options)
      : diagnostics_(diagnostics), options_(options) {}

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
    if (const auto* directive =
            result.Nodes.getNodeAs<clang::UsingDirectiveDecl>("using_namespace")) {
      if (result.SourceManager == nullptr) {
        return;
      }
      const clang::PresumedLoc presumed =
          result.SourceManager->getPresumedLoc(directive->getLocation());
      const std::string file = presumed.isInvalid() ? "" : presumed.getFilename();
      const std::string extension = std::filesystem::path(file).extension().string();
      if (extension == ".h" || extension == ".hpp" || extension == ".hh" ||
          extension == ".hxx") {
        diagnose(result, directive->getLocation(), kNoUsingNamespaceInHeader,
                 "using namespace in a header leaks names into every includer");
      }
      return;
    }
    if (const auto* call =
            result.Nodes.getNodeAs<clang::CallExpr>("sleep_in_test")) {
      const clang::FunctionDecl* callee = call->getDirectCallee();
      if (callee == nullptr || result.SourceManager == nullptr) {
        return;
      }
      const std::string_view name = callee->getName();
      if (name != "sleep" && name != "sleep_for" && name != "sleep_until" &&
          name != "usleep" && name != "nanosleep") {
        return;
      }
      const clang::SourceLocation location =
          result.SourceManager->getExpansionLoc(call->getBeginLoc());
      const clang::PresumedLoc presumed =
          result.SourceManager->getPresumedLoc(location);
      const std::string file = presumed.isInvalid() ? "" : presumed.getFilename();
      std::string lowered = file;
      std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const std::string base = std::filesystem::path(lowered).filename().string();
      const bool is_test =
          lowered.find("/tests/") != std::string::npos ||
          lowered.find("/test/") != std::string::npos ||
          base.rfind("test_", 0) == 0 || base.find("_test.") != std::string::npos ||
          base == "test.cpp";
      if (is_test) {
        diagnose(result, call->getBeginLoc(), kNoSleepInTest,
                 "sleep in a test violates the deterministic-test guideline");
      }
      return;
    }
    if (const auto* field =
            result.Nodes.getNodeAs<clang::FieldDecl>("public_data_member")) {
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus ||
          field->getAccess() != clang::AS_public) {
        return;
      }
      const auto* parent = clang::dyn_cast<clang::CXXRecordDecl>(field->getParent());
      if (parent == nullptr || !parent->isClass()) {
        return;
      }
      diagnose(result, field->getLocation(), kNoPublicDataMember,
               "public data member weakens encapsulation; use an accessor");
      return;
    }
    if (const auto* record =
            result.Nodes.getNodeAs<clang::CXXRecordDecl>("multiple_inheritance")) {
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus ||
          record->getNumBases() <= 1) {
        return;
      }
      diagnose(result, record->getLocation(), kNoMultipleInheritance,
               "multiple inheritance violates the composition guideline; prefer composition");
      return;
    }
    if (const auto* call =
            result.Nodes.getNodeAs<clang::CallExpr>("unsafe_c_function")) {
      const clang::FunctionDecl* callee = call->getDirectCallee();
      if (callee == nullptr) {
        return;
      }
      const std::string_view name = callee->getName();
      if (name != "strcpy" && name != "strcat" && name != "sprintf" &&
          name != "gets" && name != "scanf" && name != "vsprintf") {
        return;
      }
      diagnose(result, call->getBeginLoc(), kNoUnsafeCFunction,
               "unbounded C string function violates the security guideline; use a length-aware API");
      return;
    }
    if (const auto* call = result.Nodes.getNodeAs<clang::CallExpr>("weak_hash")) {
      const clang::FunctionDecl* callee = call->getDirectCallee();
      if (callee == nullptr) {
        return;
      }
      const std::string_view name = callee->getName();
      if (name != "MD5" && name != "MD5_Init" && name != "SHA1" &&
          name != "SHA1_Init") {
        return;
      }
      diagnose(result, call->getBeginLoc(), kNoWeakHash,
               "weak hash function violates the security guideline; use SHA-256 or better");
      return;
    }
    if (const auto* declaration =
            result.Nodes.getNodeAs<clang::VarDecl>("public_static_data")) {
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus ||
          !declaration->isStaticDataMember() ||
          declaration->getAccess() != clang::AS_public) {
        return;
      }
      const auto* parent =
          clang::dyn_cast<clang::CXXRecordDecl>(declaration->getDeclContext());
      if (parent == nullptr || !parent->isClass()) {
        return;
      }
      diagnose(result, declaration->getLocation(), kNoPublicStaticData,
               "public static data weakens encapsulation; use an accessor");
      return;
    }
    if (const auto* declaration =
            result.Nodes.getNodeAs<clang::VarDecl>("raw_array")) {
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus) {
        return;
      }
      diagnose(result, declaration->getLocation(), kNoRawArray,
               "raw C array violates the container guideline; use std::array or std::vector");
      return;
    }
    if (const auto* declaration =
            result.Nodes.getNodeAs<clang::FieldDecl>("raw_array")) {
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus) {
        return;
      }
      diagnose(result, declaration->getLocation(), kNoRawArray,
               "raw C array violates the container guideline; use std::array or std::vector");
      return;
    }
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CXXReinterpretCastExpr>("reinterpret")) {
      diagnose(result, expression->getBeginLoc(), kNoReinterpretCast,
               "reinterpret_cast violates the type-safety guideline; use a typed conversion");
      return;
    }
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CXXConstCastExpr>("const_cast")) {
      diagnose(result, expression->getBeginLoc(), kNoConstCast,
               "const_cast violates the type-safety guideline; keep const correctness");
      return;
    }
    if (const auto* statement =
            result.Nodes.getNodeAs<clang::GotoStmt>("goto")) {
      if (result.Context == nullptr ||
          !result.Context->getLangOpts().CPlusPlus) {
        return;
      }
      diagnose(result, statement->getGotoLoc(), kNoGoto,
               "goto violates the control-flow guideline; use structured control flow");
      return;
    }
    if (const auto* expression =
            result.Nodes.getNodeAs<clang::CStyleCastExpr>("cast")) {
      diagnose(result, expression->getBeginLoc(), kNoCStyleCast,
               "C-style cast violates the type-safety guideline; use static_cast or a named conversion");
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
    if (!options_.only_checks.empty() &&
        std::find(options_.only_checks.begin(), options_.only_checks.end(), check) ==
            options_.only_checks.end()) {
      return;
    }
    if (std::find(options_.disabled_checks.begin(), options_.disabled_checks.end(),
                  check) != options_.disabled_checks.end()) {
      return;
    }
    diagnostics_.push_back(TidyDiagnostic{check, message, presumed.getFilename(),
                                          presumed.getLine(),
                                          presumed.getColumn()});
  }

 private:

 private:
  std::vector<TidyDiagnostic>& diagnostics_;
  const TidyOptions& options_;
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
  GuardrailCallback callback(diagnostics, effective);
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
  auto cast_matcher = cStyleCastExpr(unless(isExpansionInSystemHeader()));
  auto using_namespace_matcher =
      usingDirectiveDecl(unless(isExpansionInSystemHeader()));
  auto goto_matcher = gotoStmt(unless(isExpansionInSystemHeader()));
  auto reinterpret_matcher =
      cxxReinterpretCastExpr(unless(isExpansionInSystemHeader()));
  auto const_cast_matcher =
      cxxConstCastExpr(unless(isExpansionInSystemHeader()));
  auto raw_array_matcher =
      varDecl(unless(isExpansionInSystemHeader()), hasType(arrayType()));
  auto raw_field_array_matcher =
      fieldDecl(unless(isExpansionInSystemHeader()), hasType(arrayType()));
  auto unsafe_c_matcher = callExpr(unless(isExpansionInSystemHeader()));
  auto multiple_inheritance_matcher =
      cxxRecordDecl(isDefinition(), unless(isExpansionInSystemHeader()));
  auto public_data_matcher = fieldDecl(unless(isExpansionInSystemHeader()));
  auto sleep_matcher = callExpr(unless(isExpansionInSystemHeader()));
  auto public_static_matcher = varDecl(unless(isExpansionInSystemHeader()));
  auto weak_hash_matcher = callExpr(unless(isExpansionInSystemHeader()));
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
  finder.addMatcher(std::move(cast_matcher).bind("cast"), &callback);
  finder.addMatcher(
      std::move(using_namespace_matcher).bind("using_namespace"), &callback);
  finder.addMatcher(std::move(goto_matcher).bind("goto"), &callback);
  finder.addMatcher(std::move(reinterpret_matcher).bind("reinterpret"), &callback);
  finder.addMatcher(std::move(const_cast_matcher).bind("const_cast"), &callback);
  finder.addMatcher(std::move(raw_array_matcher).bind("raw_array"), &callback);
  finder.addMatcher(std::move(raw_field_array_matcher).bind("raw_array"),
                    &callback);
  finder.addMatcher(std::move(unsafe_c_matcher).bind("unsafe_c_function"),
                    &callback);
  finder.addMatcher(std::move(multiple_inheritance_matcher)
                        .bind("multiple_inheritance"),
                    &callback);
  finder.addMatcher(std::move(public_data_matcher).bind("public_data_member"),
                    &callback);
  finder.addMatcher(std::move(sleep_matcher).bind("sleep_in_test"), &callback);
  finder.addMatcher(std::move(public_static_matcher)
                        .bind("public_static_data"),
                    &callback);
  finder.addMatcher(std::move(weak_hash_matcher).bind("weak_hash"), &callback);

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
      {"cf-swt-no-c-style-cast", "CG-TYPES",
       "C-style casts; use static_cast or a named conversion"},
      {"cf-swt-no-using-namespace-in-header", "CG-NAMESPACE",
       "using namespace directives in headers"},
      {"cf-swt-no-goto", "CG-CONTROL",
       "goto statements in C++; use structured control flow"},
      {"cf-swt-no-reinterpret-cast", "CG-TYPES",
       "reinterpret_cast; use a typed conversion"},
      {"cf-swt-no-const-cast", "CG-TYPES",
       "const_cast; keep const correctness"},
      {"cf-swt-no-raw-array", "CG-CONTAINERS",
       "raw C arrays in C++; use std::array or std::vector"},
      {"cf-swt-no-unsafe-c-function", "CG-SECURITY",
       "unbounded C string functions; use length-aware APIs"},
      {"cf-swt-no-multiple-inheritance", "CG-COMPOSITION",
       "multiple inheritance; prefer composition"},
      {"cf-swt-no-public-data-member", "CG-ENCAPSULATION",
       "public data members in classes; use accessors"},
      {"cf-swt-no-sleep-in-test", "CG-TESTS",
       "sleeps in tests; inject a clock or use synchronization"},
      {"cf-swt-no-public-static-data", "CG-ENCAPSULATION",
       "public static data in classes; use an accessor"},
      {"cf-swt-no-weak-hash", "CG-SECURITY",
       "MD5/SHA-1 calls; use SHA-256 or better"},
  };
}

}  // namespace cf::tidy
