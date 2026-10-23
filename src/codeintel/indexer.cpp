#include "cf/codeintel/indexer.hpp"

#include <map>

namespace cf::codeintel {
namespace {

// Groups a parsed graph's rows per file and writes them into the index in
// one pass. Returns the number of files written.
Status write_graph(SymbolIndex& index, const SymbolGraph& graph) {
  std::map<std::string, FileIndex> per_file;
  for (const auto& record : graph.files) {
    per_file[record.path].record = record;
  }
  for (const auto& symbol : graph.symbols) {
    per_file[symbol.file].symbols.push_back(symbol);
  }
  for (const auto& call : graph.calls) {
    per_file[call.file].calls.push_back(call);
  }
  for (const auto& include : graph.includes) {
    per_file[include.file].includes.push_back(include);
  }
  for (const auto& [path, file] : per_file) {
    auto replaced = index.replace_file(file);
    if (!replaced) {
      return replaced;
    }
  }
  return {};
}

}  // namespace

Result<std::size_t> index_tree(LanguageBackend& backend, SymbolIndex& index,
                               const std::filesystem::path& root) {
  auto graph = backend.index(root);
  if (!graph) {
    return tl::make_unexpected(graph.error());
  }
  auto written = write_graph(index, *graph);
  if (!written) {
    return tl::make_unexpected(written.error());
  }
  return graph->files.size();
}

Result<std::size_t> update_files(LanguageBackend& backend, SymbolIndex& index,
                                 const std::filesystem::path& root,
                                 const std::vector<std::filesystem::path>& changed) {
  std::size_t updated = 0;
  std::vector<std::filesystem::path> present;
  for (const auto& path : changed) {
    if (std::filesystem::is_regular_file(root / path)) {
      present.push_back(path);
      continue;
    }
    // Deleted on disk: drop it from the index (design section 6.5,
    // incremental update).
    auto removed = index.remove_file(path.string());
    if (!removed) {
      return tl::make_unexpected(removed.error());
    }
    ++updated;
  }
  if (present.empty()) {
    return updated;
  }

  SymbolGraph shell;
  shell.root = root.string();
  shell.language = std::string(backend.language());
  auto graph = backend.update(shell, present);
  if (!graph) {
    return tl::make_unexpected(graph.error());
  }
  auto written = write_graph(index, *graph);
  if (!written) {
    return tl::make_unexpected(written.error());
  }
  return updated + graph->files.size();
}

}  // namespace cf::codeintel
