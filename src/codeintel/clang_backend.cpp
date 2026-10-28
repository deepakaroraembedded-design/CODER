// Clang LibTooling back end for C and C++ (design section 6.5). The AST walk
// records declarations, call edges and include edges; the tree-sitter
// back ends remain the fallback when a project cannot be parsed (for example
// because no compilation database or toolchain headers are available).
#include "cf/codeintel/clang_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RawCommentList.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Error.h>

#include "cf/codeintel/treesitter_backend.hpp"
#include "cf/common/hash.hpp"
#include "cf/common/result.hpp"

namespace cf::codeintel {
namespace {

namespace fs = std::filesystem;

std::string truncate(std::string text, std::size_t limit) {
  if (text.size() > limit) {
    text.resize(limit);
  }
  return text;
}

bool is_translation_unit(const fs::path& path,
                         const std::vector<std::string>& extensions) {
  const std::string extension = path.extension().string();
  return std::find(extensions.begin(), extensions.end(), extension) !=
         extensions.end();
}

std::string relative_path(const clang::SourceManager& sm, clang::SourceLocation location,
                          const fs::path& root) {
  if (location.isInvalid()) {
    return {};
  }
  const clang::PresumedLoc presumed = sm.getPresumedLoc(location);
  if (presumed.isInvalid()) {
    return {};
  }
  std::error_code error;
  const fs::path absolute = fs::absolute(presumed.getFilename(), error);
  if (error) {
    return {};
  }
  const fs::path relative = fs::relative(absolute, root, error);
  if (error || relative.empty()) {
    return {};
  }
  const std::string text = relative.generic_string();
  if (text == ".." || text.rfind("../", 0) == 0) {
    return {};  // system headers and files outside the indexed root
  }
  return text;
}

std::string source_text(const clang::SourceManager& sm, clang::SourceLocation begin,
                        clang::SourceLocation end, const clang::LangOptions& lang) {
  if (begin.isInvalid() || end.isInvalid()) {
    return {};
  }
  const auto range = clang::CharSourceRange::getTokenRange(begin, end);
  const auto text = clang::Lexer::getSourceText(range, sm, lang);
  return text.str();
}

class IncludeCollector final : public clang::PPCallbacks {
 public:
  IncludeCollector(const clang::SourceManager& sm, SymbolGraph& graph, fs::path root,
                   std::set<std::string>& seen)
      : sm_(sm), graph_(graph), root_(std::move(root)), seen_(seen) {}

  void InclusionDirective(clang::SourceLocation hash_location,
                          const clang::Token&,
                          llvm::StringRef file_name,
                          bool is_angled,
                          clang::CharSourceRange,
                          clang::OptionalFileEntryRef,
                          llvm::StringRef,
                          llvm::StringRef,
                          const clang::Module*,
                          clang::SrcMgr::CharacteristicKind) override {
    const std::string file = relative_path(sm_, hash_location, root_);
    if (file.empty()) {
      return;
    }
    const clang::PresumedLoc presumed = sm_.getPresumedLoc(hash_location);
    const std::size_t line = presumed.isInvalid() ? 0 : presumed.getLine();
    const std::string key = file + "\0" + file_name.str() + "\0" +
                            std::to_string(is_angled) + "\0" +
                            std::to_string(line);
    if (!seen_.insert(key).second) {
      return;
    }
    graph_.includes.push_back(
        IncludeEdge{file, file_name.str(), is_angled, static_cast<std::uint32_t>(line)});
  }

 private:
  const clang::SourceManager& sm_;
  SymbolGraph& graph_;
  fs::path root_;
  std::set<std::string>& seen_;
};

class SymbolCollector final : public clang::RecursiveASTVisitor<SymbolCollector> {
 public:
  SymbolCollector(clang::ASTContext& context, SymbolGraph& graph, fs::path root,
                  std::map<std::string, std::size_t>& symbols,
                  std::set<std::string>& calls)
      : context_(context),
        sm_(context.getSourceManager()),
        lang_(context.getLangOpts()),
        graph_(graph),
        root_(std::move(root)),
        symbols_(symbols),
        calls_(calls) {}

  bool VisitNamespaceDecl(clang::NamespaceDecl* decl) {
    return add_symbol(decl, SymbolKind::Namespace, true);
  }

  bool VisitCXXRecordDecl(clang::CXXRecordDecl* decl) {
    if (!decl->isThisDeclarationADefinition() || decl->isImplicit()) {
      return true;
    }
    const SymbolKind kind = decl->isStruct()   ? SymbolKind::Struct
                            : decl->isUnion()  ? SymbolKind::Struct
                                               : SymbolKind::Class;
    return add_symbol(decl, kind, true);
  }

  bool VisitEnumDecl(clang::EnumDecl* decl) {
    return add_symbol(decl, SymbolKind::Enum, decl->isThisDeclarationADefinition());
  }

  bool VisitTypedefNameDecl(clang::TypedefNameDecl* decl) {
    return add_symbol(decl, SymbolKind::Typedef, true);
  }

  bool VisitFunctionDecl(clang::FunctionDecl* decl) {
    const bool definition = decl->doesThisDeclarationHaveABody();
    if (auto* method = clang::dyn_cast<clang::CXXMethodDecl>(decl)) {
      return add_symbol(method, SymbolKind::Method, definition);
    }
    return add_symbol(decl, SymbolKind::Function, definition);
  }

  bool VisitFieldDecl(clang::FieldDecl* decl) {
    return add_symbol(decl, SymbolKind::Variable, false);
  }

  bool VisitVarDecl(clang::VarDecl* decl) {
    if (!decl->isFileVarDecl()) {
      return true;
    }
    return add_symbol(decl, SymbolKind::Variable, decl->isThisDeclarationADefinition());
  }

  bool TraverseFunctionDecl(clang::FunctionDecl* decl) {
    const std::string previous = caller_;
    if (decl->doesThisDeclarationHaveABody()) {
      caller_ = decl->getQualifiedNameAsString();
    }
    const bool result = clang::RecursiveASTVisitor<SymbolCollector>::TraverseFunctionDecl(decl);
    caller_ = previous;
    return result;
  }

  bool TraverseCXXMethodDecl(clang::CXXMethodDecl* decl) {
    const std::string previous = caller_;
    if (decl->doesThisDeclarationHaveABody()) {
      caller_ = decl->getQualifiedNameAsString();
    }
    const bool result = clang::RecursiveASTVisitor<SymbolCollector>::TraverseCXXMethodDecl(decl);
    caller_ = previous;
    return result;
  }

  bool TraverseCXXConstructorDecl(clang::CXXConstructorDecl* decl) {
    const std::string previous = caller_;
    if (decl->doesThisDeclarationHaveABody()) {
      caller_ = decl->getQualifiedNameAsString();
    }
    const bool result =
        clang::RecursiveASTVisitor<SymbolCollector>::TraverseCXXConstructorDecl(decl);
    caller_ = previous;
    return result;
  }

  bool TraverseCXXDestructorDecl(clang::CXXDestructorDecl* decl) {
    const std::string previous = caller_;
    if (decl->doesThisDeclarationHaveABody()) {
      caller_ = decl->getQualifiedNameAsString();
    }
    const bool result =
        clang::RecursiveASTVisitor<SymbolCollector>::TraverseCXXDestructorDecl(decl);
    caller_ = previous;
    return result;
  }

  bool VisitCallExpr(clang::CallExpr* expr) {
    if (caller_.empty() || expr == nullptr) {
      return true;
    }
    const clang::FunctionDecl* callee = expr->getDirectCallee();
    if (callee == nullptr) {
      return true;
    }
    const std::string file = relative_path(sm_, expr->getBeginLoc(), root_);
    if (file.empty()) {
      return true;
    }
    const clang::PresumedLoc presumed = sm_.getPresumedLoc(expr->getBeginLoc());
    const std::uint32_t line =
        presumed.isInvalid() ? 0 : static_cast<std::uint32_t>(presumed.getLine());
    const std::string combined = caller_ + "\0" + callee->getNameAsString() + "\0" +
                                 file + "\0" + std::to_string(line);
    if (!calls_.insert(combined).second) {
      return true;
    }
    graph_.calls.push_back(
        CallEdge{caller_, callee->getNameAsString(), file, line});
    return true;
  }

  bool add_symbol(clang::NamedDecl* decl, SymbolKind kind, bool definition) {
    if (decl == nullptr || decl->getIdentifier() == nullptr || decl->isImplicit()) {
      return true;
    }
    const std::string file = relative_path(sm_, decl->getLocation(), root_);
    if (file.empty()) {
      return true;
    }
    const clang::PresumedLoc presumed = sm_.getPresumedLoc(decl->getLocation());
    const std::uint32_t line =
        presumed.isInvalid() ? 0 : static_cast<std::uint32_t>(presumed.getLine());
    std::string qualified = decl->getQualifiedNameAsString();
    if (qualified.empty()) {
      qualified = decl->getNameAsString();
    }
    clang::SourceLocation end = decl->getEndLoc();
    if (auto* function = clang::dyn_cast<clang::FunctionDecl>(decl)) {
      if (function->doesThisDeclarationHaveABody()) {
        end = function->getBody()->getBeginLoc();
      }
    } else if (auto* record = clang::dyn_cast<clang::CXXRecordDecl>(decl)) {
      if (record->isCompleteDefinition()) {
        end = record->getBraceRange().getBegin();
      }
    }
    std::string signature = source_text(sm_, decl->getBeginLoc(), end, lang_);
    while (!signature.empty() &&
           (signature.back() == ';' || signature.back() == '{' ||
            std::isspace(static_cast<unsigned char>(signature.back())))) {
      signature.pop_back();
    }
    if (signature.empty()) {
      signature = qualified;
    }

    std::string doc;
    if (const clang::RawComment* comment =
            context_.getRawCommentForDeclNoCache(decl)) {
      doc = comment->getRawText(sm_).str();
    }

    // Redeclarations (a forward declaration followed by its definition) share
    // a key; keep the definition and merge documentation.
    const std::string key = qualified + "\0" + file + "\0" +
                            std::to_string(static_cast<int>(kind)) + "\0" +
                            signature;
    auto existing = symbols_.find(key);
    if (existing != symbols_.end()) {
      Symbol& symbol = graph_.symbols[existing->second];
      if (definition && !symbol.is_definition) {
        symbol.line = line;
        symbol.is_definition = true;
      }
      if (!doc.empty() && (symbol.doc.empty() || definition)) {
        symbol.doc = truncate(std::move(doc), 1024);
      }
      return true;
    }
    symbols_.emplace(key, graph_.symbols.size());
    graph_.symbols.push_back(Symbol{
        decl->getNameAsString(), std::move(qualified), kind, file, line,
        truncate(std::move(signature), 512), truncate(std::move(doc), 1024),
        definition});
    return true;
  }

 private:
  clang::ASTContext& context_;
  const clang::SourceManager& sm_;
  const clang::LangOptions& lang_;
  SymbolGraph& graph_;
  fs::path root_;
  std::map<std::string, std::size_t>& symbols_;
  std::set<std::string>& calls_;
  std::string caller_;
};

class ClangAstConsumer final : public clang::ASTConsumer {
 public:
  ClangAstConsumer(SymbolGraph& graph, fs::path root,
                   std::map<std::string, std::size_t>& symbols,
                   std::set<std::string>& calls)
      : graph_(graph),
        root_(std::move(root)),
        symbols_(symbols),
        calls_(calls) {}

  void HandleTranslationUnit(clang::ASTContext& context) override {
    SymbolCollector collector(context, graph_, root_, symbols_, calls_);
    collector.TraverseDecl(context.getTranslationUnitDecl());
  }

 private:
  SymbolGraph& graph_;
  fs::path root_;
  std::map<std::string, std::size_t>& symbols_;
  std::set<std::string>& calls_;
};

class ClangFrontendAction final : public clang::ASTFrontendAction {
 public:
  ClangFrontendAction(SymbolGraph& graph, fs::path root,
                      std::map<std::string, std::size_t>& symbols,
                      std::set<std::string>& calls,
                      std::set<std::string>& includes)
      : graph_(graph),
        root_(std::move(root)),
        symbols_(symbols),
        calls_(calls),
        includes_(includes) {}

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance&, llvm::StringRef) override {
    return std::make_unique<ClangAstConsumer>(graph_, root_, symbols_, calls_);
  }

  bool BeginSourceFileAction(clang::CompilerInstance& compiler) override {
    compiler.getPreprocessor().addPPCallbacks(std::make_unique<IncludeCollector>(
        compiler.getSourceManager(), graph_, root_, includes_));
    return true;
  }

 private:
  SymbolGraph& graph_;
  fs::path root_;
  std::map<std::string, std::size_t>& symbols_;
  std::set<std::string>& calls_;
  std::set<std::string>& includes_;
};

class ClangFrontendActionFactory final : public clang::tooling::FrontendActionFactory {
 public:
  ClangFrontendActionFactory(SymbolGraph& graph, fs::path root,
                             std::map<std::string, std::size_t>& symbols,
                             std::set<std::string>& calls,
                             std::set<std::string>& includes)
      : graph_(graph),
        root_(std::move(root)),
        symbols_(symbols),
        calls_(calls),
        includes_(includes) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<ClangFrontendAction>(graph_, root_, symbols_, calls_,
                                                 includes_);
  }

 private:
  SymbolGraph& graph_;
  fs::path root_;
  std::map<std::string, std::size_t>& symbols_;
  std::set<std::string>& calls_;
  std::set<std::string>& includes_;
};

std::vector<fs::path> collect_files(const fs::path& root,
                                    const std::vector<std::string>& extensions,
                                    bool translation_units_only) {
  std::vector<fs::path> files;
  std::error_code error;
  fs::recursive_directory_iterator iterator(
      root, fs::directory_options::skip_permission_denied, error);
  const fs::recursive_directory_iterator end;
  while (!error && iterator != end) {
    const fs::path path = iterator->path();
    if (iterator->is_directory(error)) {
      const std::string name = path.filename().string();
      if (name == ".git" || name == "build" || name == ".cache" ||
          name == "node_modules" || name == "target" || name == ".venv") {
        iterator.disable_recursion_pending();
      }
    } else if (iterator->is_regular_file(error) &&
               is_translation_unit(path, extensions) &&
               (!translation_units_only ||
                (path.extension() != ".h" && path.extension() != ".hpp" &&
                 path.extension() != ".hh" && path.extension() != ".hxx"))) {
      files.push_back(path);
    }
    iterator.increment(error);
  }
  std::sort(files.begin(), files.end());
  return files;
}

void add_file_record(SymbolGraph& graph, const fs::path& root,
                     const fs::path& file) {
  std::ifstream input(file, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  std::error_code error;
  const fs::path relative = fs::relative(fs::absolute(file, error), root, error);
  if (error) {
    return;
  }
  graph.files.push_back(FileRecord{relative.generic_string(), sha256_hex(contents)});
}

class ClangBackend final : public LanguageBackend {
 public:
  ClangBackend(std::string language, std::vector<std::string> extensions)
      : language_(std::move(language)),
        extensions_(std::move(extensions)),
        fallback_(language_ == "c" ? make_c_backend() : make_cpp_backend()) {}

  std::string_view language() const override { return language_; }

  Result<SymbolGraph> index(const fs::path& root) override {
    std::error_code error;
    const fs::path absolute = fs::absolute(root, error);
    if (error) {
      return fail(error_codes::kParseFailed,
                  "cannot resolve code intelligence root: " + error.message());
    }
    const auto all_files = collect_files(absolute, extensions_, false);
    const auto source_files = collect_files(absolute, extensions_, true);
    if (source_files.empty()) {
      return fallback_->index(absolute);
    }

    SymbolGraph graph;
    graph.root = absolute.generic_string();
    graph.language = language_;
    for (const auto& file : all_files) {
      add_file_record(graph, absolute, file);
    }

    auto parsed = parse(absolute, source_files, graph);
    if (!parsed) {
      return tl::make_unexpected(parsed.error());
    }
    if (graph.symbols.empty()) {
      auto fallback = fallback_->index(absolute);
      if (fallback) {
        return fallback;
      }
      return graph;
    }
    return graph;
  }

  Result<SymbolGraph> update(
      const SymbolGraph& graph,
      const std::vector<fs::path>& changed) override {
    const fs::path root(graph.root);
    std::vector<fs::path> files;
    bool has_header = false;
    for (const auto& relative : changed) {
      const fs::path path = root / relative;
      if (!fs::is_regular_file(path)) {
        continue;
      }
      if (!is_translation_unit(path, extensions_)) {
        continue;
      }
      if (path.extension() == ".h" || path.extension() == ".hpp" ||
          path.extension() == ".hh" || path.extension() == ".hxx") {
        has_header = true;
      }
      files.push_back(path);
    }
    if (files.empty()) {
      return fallback_->update(graph, changed);
    }
    // Header changes can affect any translation unit; a full Clang pass is
    // cheaper than guessing which TU includes the header.
    if (has_header) {
      return index(root);
    }

    SymbolGraph result;
    result.root = graph.root;
    result.language = language_;
    for (const auto& file : files) {
      add_file_record(result, root, file);
    }
    auto parsed = parse(root, files, result);
    if (!parsed) {
      return tl::make_unexpected(parsed.error());
    }
    std::set<std::string> changed_paths;
    for (const auto& file : files) {
      std::error_code error;
      const fs::path relative = fs::relative(file, root, error);
      if (!error) {
        changed_paths.insert(relative.generic_string());
      }
    }
    result.symbols.erase(
        std::remove_if(result.symbols.begin(), result.symbols.end(),
                       [&](const Symbol& symbol) {
                         return changed_paths.count(symbol.file) == 0;
                       }),
        result.symbols.end());
    result.calls.erase(
        std::remove_if(result.calls.begin(), result.calls.end(),
                       [&](const CallEdge& call) {
                         return changed_paths.count(call.file) == 0;
                       }),
        result.calls.end());
    result.includes.erase(
        std::remove_if(result.includes.begin(), result.includes.end(),
                       [&](const IncludeEdge& include) {
                         return changed_paths.count(include.file) == 0;
                       }),
        result.includes.end());
    result.files.erase(
        std::remove_if(result.files.begin(), result.files.end(),
                       [&](const FileRecord& record) {
                         return changed_paths.count(record.path) == 0;
                       }),
        result.files.end());
    if (result.symbols.empty()) {
      auto fallback = fallback_->update(graph, changed);
      if (fallback) {
        return fallback;
      }
      return result;
    }
    return result;
  }

  Result<std::vector<Diagnostic>> check_architecture(
      const SymbolGraph& graph, const ArchitectureRules& rules) override {
    return check_layer_rules(graph, rules);
  }

 private:
  Result<Status> parse(const fs::path& root,
                       const std::vector<fs::path>& files,
                       SymbolGraph& graph) {
    std::vector<std::string> paths;
    paths.reserve(files.size());
    for (const auto& file : files) {
      paths.push_back(file.generic_string());
    }

    std::unique_ptr<clang::tooling::CompilationDatabase> database;
    std::string error;
    database = clang::tooling::CompilationDatabase::autoDetectFromDirectory(
        root.generic_string(), error);
    if (!database) {
      std::vector<std::string> flags;
      if (language_ == "c") {
        flags = {"-x", "c", "-std=c11", "-fsyntax-only"};
      } else {
        flags = {"-x", "c++", "-std=c++17", "-fsyntax-only"};
      }
      flags.push_back("-I" + root.generic_string());
      flags.push_back("-I" + (root / "include").generic_string());
      flags.push_back("-I" + (root / "src").generic_string());
#ifdef CF_CLANG_RESOURCE_DIR
      flags.push_back("-resource-dir=" CF_CLANG_RESOURCE_DIR);
#endif
      database = std::make_unique<clang::tooling::FixedCompilationDatabase>(
          ".", std::move(flags));
    }

    std::map<std::string, std::size_t> symbols;
    std::set<std::string> calls;
    std::set<std::string> includes;
    clang::tooling::ClangTool tool(*database, paths);
    tool.appendArgumentsAdjuster(clang::tooling::getClangSyntaxOnlyAdjuster());
#ifdef CF_CLANG_RESOURCE_DIR
    // The in-process driver cannot infer its resource directory from argv[0].
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        "-resource-dir=" CF_CLANG_RESOURCE_DIR,
        clang::tooling::ArgumentInsertPosition::END));
#endif
    // RawCommentList only attaches ordinary // comments when this flag is
    // present; doc comments are part of the symbol graph.
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        "-fparse-all-comments", clang::tooling::ArgumentInsertPosition::END));
    ClangFrontendActionFactory factory(graph, root, symbols, calls, includes);
    clang::IgnoringDiagConsumer diagnostics;
    tool.setDiagnosticConsumer(&diagnostics);
    tool.run(&factory);
    return Status{};
  }

  std::string language_;
  std::vector<std::string> extensions_;
  std::unique_ptr<LanguageBackend> fallback_;
};

}  // namespace

std::unique_ptr<LanguageBackend> make_clang_backend(
    std::string language, std::vector<std::string> extensions) {
  return std::make_unique<ClangBackend>(std::move(language), std::move(extensions));
}

}  // namespace cf::codeintel
