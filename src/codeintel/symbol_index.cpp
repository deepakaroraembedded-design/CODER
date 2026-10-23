#include "cf/codeintel/symbol_index.hpp"

#include <exception>
#include <optional>
#include <utility>

#include <SQLiteCpp/SQLiteCpp.h>

namespace cf::codeintel {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS files (
  path    TEXT PRIMARY KEY,
  sha256  TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS symbols (
  file          TEXT NOT NULL,
  line          INT NOT NULL,
  name          TEXT NOT NULL,
  qualified     TEXT NOT NULL,
  kind          TEXT NOT NULL,
  signature     TEXT NOT NULL,
  doc           TEXT NOT NULL,
  is_definition INT NOT NULL
);
CREATE INDEX IF NOT EXISTS symbols_name ON symbols (name);
CREATE INDEX IF NOT EXISTS symbols_qualified ON symbols (qualified);
CREATE TABLE IF NOT EXISTS calls (
  caller TEXT NOT NULL,
  callee TEXT NOT NULL,
  file   TEXT NOT NULL,
  line   INT NOT NULL
);
CREATE INDEX IF NOT EXISTS calls_callee ON calls (callee);
CREATE INDEX IF NOT EXISTS calls_caller ON calls (caller);
CREATE TABLE IF NOT EXISTS includes (
  file     TEXT NOT NULL,
  included TEXT NOT NULL,
  system   INT NOT NULL,
  line     INT NOT NULL
);
CREATE TABLE IF NOT EXISTS meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
)sql";

// SQLiteCpp throws; the module boundary speaks Status (design section 4.1).
template <typename F>
auto sql_guard(std::string_view what, F&& fn) -> decltype(fn()) {
  try {
    return fn();
  } catch (const std::exception& e) {
    return fail(error_codes::kIndexQueryFailed,
                std::string(what) + ": " + e.what());
  }
}

}  // namespace

struct SymbolIndex::Impl {
  explicit Impl(const std::filesystem::path& db_path)
      : db(db_path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {}
  SQLite::Database db;
};

SymbolIndex::SymbolIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SymbolIndex::~SymbolIndex() = default;
SymbolIndex::SymbolIndex(SymbolIndex&&) noexcept = default;
SymbolIndex& SymbolIndex::operator=(SymbolIndex&&) noexcept = default;

Result<std::unique_ptr<SymbolIndex>> SymbolIndex::open(
    const std::filesystem::path& db_path) {
  try {
    auto impl = std::make_unique<Impl>(db_path);
    impl->db.exec("PRAGMA journal_mode=WAL;");
    impl->db.exec("PRAGMA busy_timeout=5000;");
    impl->db.exec(kSchema);
    return std::unique_ptr<SymbolIndex>(new SymbolIndex(std::move(impl)));
  } catch (const std::exception& e) {
    return fail(error_codes::kIndexOpenFailed, db_path.string() + ": " + e.what());
  }
}

Status SymbolIndex::replace_file(const FileIndex& file) {
  return sql_guard("replace_file", [&]() -> Status {
    SQLite::Transaction transaction(impl_->db);

    for (const char* clear : {"DELETE FROM symbols WHERE file = ?",
                              "DELETE FROM calls WHERE file = ?",
                              "DELETE FROM includes WHERE file = ?"}) {
      SQLite::Statement stmt(impl_->db, clear);
      stmt.bind(1, file.record.path);
      stmt.exec();
    }

    {
      SQLite::Statement stmt(
          impl_->db, "INSERT OR REPLACE INTO files (path, sha256) VALUES (?, ?)");
      stmt.bind(1, file.record.path);
      stmt.bind(2, file.record.sha256);
      stmt.exec();
    }

    for (const auto& symbol : file.symbols) {
      SQLite::Statement stmt(
          impl_->db,
          "INSERT INTO symbols (file, line, name, qualified, kind, signature, doc,"
          " is_definition) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
      stmt.bind(1, symbol.file);
      stmt.bind(2, static_cast<int64_t>(symbol.line));
      stmt.bind(3, symbol.name);
      stmt.bind(4, symbol.qualified);
      stmt.bind(5, std::string(to_string(symbol.kind)));
      stmt.bind(6, symbol.signature);
      stmt.bind(7, symbol.doc);
      stmt.bind(8, symbol.is_definition ? 1 : 0);
      stmt.exec();
    }

    for (const auto& call : file.calls) {
      SQLite::Statement stmt(
          impl_->db,
          "INSERT INTO calls (caller, callee, file, line) VALUES (?, ?, ?, ?)");
      stmt.bind(1, call.caller);
      stmt.bind(2, call.callee);
      stmt.bind(3, call.file);
      stmt.bind(4, static_cast<int64_t>(call.line));
      stmt.exec();
    }

    for (const auto& include : file.includes) {
      SQLite::Statement stmt(
          impl_->db,
          "INSERT INTO includes (file, included, system, line) VALUES (?, ?, ?, ?)");
      stmt.bind(1, include.file);
      stmt.bind(2, include.included);
      stmt.bind(3, include.system ? 1 : 0);
      stmt.bind(4, static_cast<int64_t>(include.line));
      stmt.exec();
    }

    transaction.commit();
    return {};
  });
}

Status SymbolIndex::remove_file(std::string_view path) {
  return sql_guard("remove_file", [&]() -> Status {
    SQLite::Transaction transaction(impl_->db);
    for (const char* clear : {"DELETE FROM symbols WHERE file = ?",
                              "DELETE FROM calls WHERE file = ?",
                              "DELETE FROM includes WHERE file = ?",
                              "DELETE FROM files WHERE path = ?"}) {
      SQLite::Statement stmt(impl_->db, clear);
      stmt.bind(1, std::string(path));
      stmt.exec();
    }
    transaction.commit();
    return {};
  });
}

Result<std::vector<Symbol>> SymbolIndex::find_symbol(std::string_view name) const {
  return sql_guard("find_symbol", [&]() -> Result<std::vector<Symbol>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT file, line, name, qualified, kind, signature, doc, is_definition "
        "FROM symbols WHERE name = ? OR qualified = ? "
        "ORDER BY is_definition DESC, file, line");
    stmt.bind(1, std::string(name));
    stmt.bind(2, std::string(name));
    std::vector<Symbol> symbols;
    while (stmt.executeStep()) {
      Symbol symbol;
      symbol.file = stmt.getColumn(0).getString();
      symbol.line = static_cast<std::uint32_t>(stmt.getColumn(1).getInt64());
      symbol.name = stmt.getColumn(2).getString();
      symbol.qualified = stmt.getColumn(3).getString();
      symbol.kind = symbol_kind_from_string(stmt.getColumn(4).getString())
                        .value_or(SymbolKind::Function);
      symbol.signature = stmt.getColumn(5).getString();
      symbol.doc = stmt.getColumn(6).getString();
      symbol.is_definition = stmt.getColumn(7).getInt() != 0;
      symbols.push_back(std::move(symbol));
    }
    return symbols;
  });
}

Result<std::vector<Symbol>> SymbolIndex::symbols_under(std::string_view prefix) const {
  return sql_guard("symbols_under", [&]() -> Result<std::vector<Symbol>> {
    // Prefixes apply on a path boundary: "src" matches "src/a.c" but not
    // "srcx/a.c"; a component stem matches its files: "src/registry" matches
    // "src/registry.h" and "src/registry.c".
    std::string directory(prefix);
    if (!directory.empty() && directory.back() != '/') {
      directory += '/';
    }
    const std::string stem = std::string(prefix) + ".";
    SQLite::Statement stmt(
        impl_->db,
        "SELECT file, line, name, qualified, kind, signature, doc, is_definition "
        "FROM symbols WHERE file = ? OR substr(file, 1, ?) = ? "
        "OR substr(file, 1, ?) = ? "
        "ORDER BY file, line");
    stmt.bind(1, std::string(prefix));
    stmt.bind(2, static_cast<int64_t>(directory.size()));
    stmt.bind(3, directory);
    stmt.bind(4, static_cast<int64_t>(stem.size()));
    stmt.bind(5, stem);
    std::vector<Symbol> symbols;
    while (stmt.executeStep()) {
      Symbol symbol;
      symbol.file = stmt.getColumn(0).getString();
      symbol.line = static_cast<std::uint32_t>(stmt.getColumn(1).getInt64());
      symbol.name = stmt.getColumn(2).getString();
      symbol.qualified = stmt.getColumn(3).getString();
      symbol.kind = symbol_kind_from_string(stmt.getColumn(4).getString())
                        .value_or(SymbolKind::Function);
      symbol.signature = stmt.getColumn(5).getString();
      symbol.doc = stmt.getColumn(6).getString();
      symbol.is_definition = stmt.getColumn(7).getInt() != 0;
      symbols.push_back(std::move(symbol));
    }
    return symbols;
  });
}

Result<std::vector<CallEdge>> SymbolIndex::get_callers(std::string_view symbol) const {
  return sql_guard("get_callers", [&]() -> Result<std::vector<CallEdge>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT caller, callee, file, line FROM calls WHERE callee = ? "
        "ORDER BY file, line");
    stmt.bind(1, std::string(symbol));
    std::vector<CallEdge> callers;
    while (stmt.executeStep()) {
      CallEdge edge;
      edge.caller = stmt.getColumn(0).getString();
      edge.callee = stmt.getColumn(1).getString();
      edge.file = stmt.getColumn(2).getString();
      edge.line = static_cast<std::uint32_t>(stmt.getColumn(3).getInt64());
      callers.push_back(std::move(edge));
    }
    return callers;
  });
}

Result<std::vector<CallEdge>> SymbolIndex::get_callees(std::string_view caller) const {
  return sql_guard("get_callees", [&]() -> Result<std::vector<CallEdge>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT caller, callee, file, line FROM calls WHERE caller = ? "
        "ORDER BY file, line");
    stmt.bind(1, std::string(caller));
    std::vector<CallEdge> callees;
    while (stmt.executeStep()) {
      CallEdge edge;
      edge.caller = stmt.getColumn(0).getString();
      edge.callee = stmt.getColumn(1).getString();
      edge.file = stmt.getColumn(2).getString();
      edge.line = static_cast<std::uint32_t>(stmt.getColumn(3).getInt64());
      callees.push_back(std::move(edge));
    }
    return callees;
  });
}

Status SymbolIndex::set_meta(std::string_view key, std::string_view value) {
  return sql_guard("set_meta", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
    stmt.bind(1, std::string(key));
    stmt.bind(2, std::string(value));
    stmt.exec();
    return {};
  });
}

Result<std::optional<std::string>> SymbolIndex::get_meta(std::string_view key) const {
  return sql_guard("get_meta", [&]() -> Result<std::optional<std::string>> {
    SQLite::Statement stmt(impl_->db, "SELECT value FROM meta WHERE key = ?");
    stmt.bind(1, std::string(key));
    if (stmt.executeStep()) {
      return std::optional<std::string>(stmt.getColumn(0).getString());
    }
    return std::optional<std::string>();
  });
}

}  // namespace cf::codeintel
