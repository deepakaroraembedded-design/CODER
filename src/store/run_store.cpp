#include "cf/store/run_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <sqlite3.h>

#include <SQLiteCpp/SQLiteCpp.h>

namespace cf::store {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS runs (
  run_id      TEXT PRIMARY KEY,
  plan_id     TEXT,
  state       TEXT,
  created_at  INT,
  config_json TEXT
);
CREATE TABLE IF NOT EXISTS transitions (
  id          INTEGER PRIMARY KEY,
  run_id      TEXT,
  from_state  TEXT,
  to_state    TEXT,
  event_json  TEXT,
  at          INT
);
CREATE TABLE IF NOT EXISTS milestones (
  run_id       TEXT,
  milestone_id TEXT,
  state        TEXT,
  iterations   INT,
  PRIMARY KEY (run_id, milestone_id)
);
CREATE TABLE IF NOT EXISTS llm_calls (
  id                INTEGER PRIMARY KEY,
  run_id            TEXT,
  role              TEXT,
  model             TEXT,
  prompt_tokens     INT,
  completion_tokens INT,
  cached_tokens     INT,
  usd               REAL,
  latency_ms        INT,
  prompt_ref        TEXT,
  response_ref      TEXT,
  at                INT
);
CREATE TABLE IF NOT EXISTS gate_results (
  id           INTEGER PRIMARY KEY,
  run_id       TEXT,
  milestone_id TEXT,
  commit_sha   TEXT,
  gate         TEXT,
  passed       INT,
  report_ref   TEXT,
  at           INT
);
CREATE TABLE IF NOT EXISTS defects (
  id          TEXT PRIMARY KEY,
  run_id      TEXT,
  milestone_id TEXT,
  status      TEXT,
  severity    TEXT,
  fingerprint TEXT,
  body_json   TEXT,
  attempts    INT
);
CREATE TABLE IF NOT EXISTS doc_versions (
  doc_id      TEXT,
  version     TEXT,
  sha256      TEXT,
  precedence  INT,
  ingested_at INT,
  PRIMARY KEY (doc_id, version)
);
-- Side-effect protocol (design section 6.12)
CREATE TABLE IF NOT EXISTS operations (
  op_id           TEXT PRIMARY KEY,
  idempotency_key TEXT UNIQUE,
  run_id          TEXT,
  milestone_id    TEXT,
  kind            TEXT,
  state           TEXT,
  input_json      TEXT,
  expected_json   TEXT,
  result_json     TEXT,
  attempts        INT,
  epoch           INT,
  created_at      INT,
  updated_at      INT
);
CREATE TABLE IF NOT EXISTS checkpoints (
  id                  INTEGER PRIMARY KEY,
  run_id              TEXT,
  milestone_id        TEXT,
  worktree_heads_json TEXT,
  sessions_json       TEXT,
  milestone_state     TEXT,
  at                  INT
);
CREATE TABLE IF NOT EXISTS run_lease (
  run_id     TEXT PRIMARY KEY,
  holder     TEXT,
  epoch      INT,
  renewed_at INT
);
-- Parallel milestones (design section 6.13)
CREATE TABLE IF NOT EXISTS locks (
  lock_id          TEXT PRIMARY KEY,
  run_id           TEXT,
  kind             TEXT,
  holder_milestone TEXT,
  epoch            INT,
  acquired_at      INT
);
CREATE TABLE IF NOT EXISTS reviews (
  review_id       TEXT PRIMARY KEY,
  run_id          TEXT,
  milestone_id    TEXT,
  commit_sha      TEXT,
  kind            TEXT,
  base_review_id  TEXT,
  verdict         TEXT,
  file_hashes_json TEXT,
  at              INT
);
CREATE INDEX IF NOT EXISTS idx_transitions_run ON transitions(run_id);
CREATE INDEX IF NOT EXISTS idx_operations_run_state ON operations(run_id, state);
CREATE INDEX IF NOT EXISTS idx_llm_calls_run ON llm_calls(run_id);
CREATE INDEX IF NOT EXISTS idx_checkpoints_run ON checkpoints(run_id, milestone_id);
)sql";

constexpr const char* kOperationColumns =
    "op_id, idempotency_key, run_id, milestone_id, kind, state, input_json, "
    "expected_json, result_json, attempts, epoch, created_at, updated_at";

// Catches SQLite and filesystem exceptions at the module boundary and
// converts them to cf::Error (design section 4.1).
template <typename F>
auto sql_guard(std::string_view context, F&& fn) -> decltype(fn()) {
  try {
    return fn();
  } catch (const std::exception& e) {
    return fail(error_codes::kQueryFailed, std::string(context) + ": " + e.what());
  }
}

OperationRecord read_operation(const SQLite::Statement& stmt) {
  OperationRecord op;
  op.op_id = stmt.getColumn("op_id").getString();
  op.idempotency_key = stmt.getColumn("idempotency_key").getString();
  op.run_id = stmt.getColumn("run_id").getString();
  op.milestone_id = stmt.getColumn("milestone_id").getString();
  op.kind = stmt.getColumn("kind").getString();
  op.state = stmt.getColumn("state").getString();
  op.input_json = stmt.getColumn("input_json").getString();
  op.expected_json = stmt.getColumn("expected_json").getString();
  op.result_json = stmt.getColumn("result_json").getString();
  op.attempts = static_cast<uint32_t>(stmt.getColumn("attempts").getInt());
  op.epoch = static_cast<uint64_t>(stmt.getColumn("epoch").getInt64());
  op.created_at = stmt.getColumn("created_at").getInt64();
  op.updated_at = stmt.getColumn("updated_at").getInt64();
  return op;
}

Result<OperationRecord> load_operation(SQLite::Database& db, std::string_view op_id) {
  SQLite::Statement stmt(
      db, std::string("SELECT ") + kOperationColumns + " FROM operations WHERE op_id = ?");
  stmt.bind(1, std::string(op_id));
  if (!stmt.executeStep()) {
    return fail(error_codes::kNotFound, "operation not found: " + std::string(op_id));
  }
  return read_operation(stmt);
}

// The current lease epoch of a run; 0 when no lease row exists.
Result<uint64_t> current_epoch(SQLite::Database& db, std::string_view run_id) {
  SQLite::Statement stmt(db, "SELECT epoch FROM run_lease WHERE run_id = ?");
  stmt.bind(1, std::string(run_id));
  if (!stmt.executeStep()) {
    return uint64_t{0};
  }
  return static_cast<uint64_t>(stmt.getColumn(0).getInt64());
}

// An operation may only be started or confirmed under the epoch that holds
// the lease (design section 6.12.6): a stale dispatcher must be refused.
Status check_epoch(SQLite::Database& db, std::string_view run_id, uint64_t epoch) {
  auto current = current_epoch(db, run_id);
  if (!current) {
    return tl::make_unexpected(current.error());
  }
  if (*current != epoch) {
    return fail(error_codes::kStaleEpoch,
                "operation epoch " + std::to_string(epoch) +
                    " does not match current lease epoch " +
                    std::to_string(*current) + " for run " + std::string(run_id));
  }
  return {};
}

Status insert_operation(SQLite::Database& db, std::string_view run_id,
                        const NewOperation& op, UnixTime now) {
  const std::string op_id = op.op_id.empty() ? new_op_id() : op.op_id;
  SQLite::Statement stmt(
      db,
      "INSERT INTO operations (op_id, idempotency_key, run_id, milestone_id, kind, "
      "state, input_json, expected_json, result_json, attempts, epoch, created_at, "
      "updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, '', 0, 0, ?, ?)");
  stmt.bind(1, op_id);
  stmt.bind(2, op.idempotency_key);
  stmt.bind(3, std::string(run_id));
  stmt.bind(4, op.milestone_id);
  stmt.bind(5, op.kind);
  stmt.bind(6, op_state::kPending);
  stmt.bind(7, op.input_json);
  stmt.bind(8, op.expected_json);
  stmt.bind(9, static_cast<int64_t>(now));
  stmt.bind(10, static_cast<int64_t>(now));
  try {
    stmt.exec();
  } catch (const SQLite::Exception& e) {
    if (e.getErrorCode() == SQLITE_CONSTRAINT) {
      return fail(error_codes::kConflict,
                  "idempotency key already recorded: " + op.idempotency_key);
    }
    throw;
  }
  return {};
}

// The state-transition half of apply_transition, shared with
// confirm_operation_with_transition; runs inside the caller's transaction.
Status apply_transition_in_tx(SQLite::Database& db, std::string_view run_id,
                              std::string_view from_state, std::string_view to_state,
                              std::string_view event_json,
                              const std::vector<NewOperation>& intents, UnixTime now) {
  SQLite::Statement current(db, "SELECT state FROM runs WHERE run_id = ?");
  current.bind(1, std::string(run_id));
  if (!current.executeStep()) {
    return fail(error_codes::kNotFound, "run not found: " + std::string(run_id));
  }
  const std::string stored = current.getColumn(0).getString();
  if (stored != from_state) {
    return fail(error_codes::kInvalidTransition,
                "run " + std::string(run_id) + " is in state " + stored +
                    ", expected " + std::string(from_state));
  }

  for (const auto& intent : intents) {
    auto inserted = insert_operation(db, run_id, intent, now);
    if (!inserted) {
      return inserted;
    }
  }

  SQLite::Statement update(db, "UPDATE runs SET state = ? WHERE run_id = ?");
  update.bind(1, std::string(to_state));
  update.bind(2, std::string(run_id));
  update.exec();

  SQLite::Statement log(
      db, "INSERT INTO transitions (run_id, from_state, to_state, event_json, at) "
          "VALUES (?, ?, ?, ?, ?)");
  log.bind(1, std::string(run_id));
  log.bind(2, std::string(from_state));
  log.bind(3, std::string(to_state));
  log.bind(4, std::string(event_json));
  log.bind(5, static_cast<int64_t>(now));
  log.exec();
  return {};
}

Status update_operation_state(SQLite::Database& db, std::string_view op_id,
                              std::string_view from_state, std::string_view to_state,
                              std::string_view result_json, UnixTime now) {
  SQLite::Statement stmt(
      db,
      "UPDATE operations SET state = ?, result_json = ?, updated_at = ? "
      "WHERE op_id = ? AND state = ?");
  stmt.bind(1, std::string(to_state));
  stmt.bind(2, std::string(result_json));
  stmt.bind(3, static_cast<int64_t>(now));
  stmt.bind(4, std::string(op_id));
  stmt.bind(5, std::string(from_state));
  if (stmt.exec() == 0) {
    return fail(error_codes::kInvalidOperationState,
                "operation " + std::string(op_id) + " is not in state " +
                    std::string(from_state));
  }
  return {};
}

}  // namespace

UnixTime unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string new_op_id() {
  unsigned char bytes[8] = {0};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    std::random_device rd;
    for (auto& b : bytes) {
      b = static_cast<unsigned char>(rd());
    }
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "op_";
  out.reserve(2 + sizeof(bytes) * 2);
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

struct RunStore::Impl {
  Impl(const std::filesystem::path& db_path)
      : db(db_path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE),
        lock_path(db_path.string() + ".lease") {}

  ~Impl() {
    if (lock_fd >= 0) {
      ::flock(lock_fd, LOCK_UN);
      ::close(lock_fd);
    }
  }

  // Takes the exclusive file lock on the run directory (design section
  // 6.12.6); held for the lifetime of the store or until release_lease.
  Status ensure_lock() {
    if (lock_fd >= 0) {
      return {};
    }
    const int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
      return fail(error_codes::kFileLockFailed,
                  "open " + lock_path + ": " + std::strerror(errno));
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int err = errno;
      ::close(fd);
      if (err == EWOULDBLOCK) {
        return fail(error_codes::kLeaseHeld, "file lock is held by another process");
      }
      return fail(error_codes::kFileLockFailed,
                  "flock " + lock_path + ": " + std::strerror(err));
    }
    lock_fd = fd;
    return {};
  }

  SQLite::Database db;
  std::string lock_path;
  int lock_fd = -1;
};

RunStore::RunStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RunStore::~RunStore() = default;

Result<std::unique_ptr<RunStore>> RunStore::open(const std::filesystem::path& db_path) {
  try {
    auto impl = std::make_unique<Impl>(db_path);
    impl->db.exec("PRAGMA journal_mode=WAL;");
    impl->db.exec("PRAGMA busy_timeout=5000;");
    impl->db.exec("PRAGMA foreign_keys=ON;");
    impl->db.exec(kSchema);
    return std::unique_ptr<RunStore>(new RunStore(std::move(impl)));
  } catch (const std::exception& e) {
    return fail(error_codes::kOpenFailed, db_path.string() + ": " + e.what());
  }
}

Status RunStore::create_run(const RunRecord& run) {
  return sql_guard("create_run", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO runs (run_id, plan_id, state, created_at, config_json) "
        "VALUES (?, ?, ?, ?, ?)");
    stmt.bind(1, run.run_id);
    stmt.bind(2, run.plan_id);
    stmt.bind(3, run.state);
    stmt.bind(4, static_cast<int64_t>(run.created_at));
    stmt.bind(5, run.config_json);
    try {
      stmt.exec();
    } catch (const SQLite::Exception& e) {
      if (e.getErrorCode() == SQLITE_CONSTRAINT) {
        return fail(error_codes::kConflict, "run already exists: " + run.run_id);
      }
      throw;
    }
    return {};
  });
}

Result<RunRecord> RunStore::get_run(std::string_view run_id) const {
  return sql_guard("get_run", [&]() -> Result<RunRecord> {
    SQLite::Statement stmt(
        impl_->db, "SELECT run_id, plan_id, state, created_at, config_json FROM runs "
                   "WHERE run_id = ?");
    stmt.bind(1, std::string(run_id));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound, "run not found: " + std::string(run_id));
    }
    RunRecord run;
    run.run_id = stmt.getColumn("run_id").getString();
    run.plan_id = stmt.getColumn("plan_id").getString();
    run.state = stmt.getColumn("state").getString();
    run.created_at = stmt.getColumn("created_at").getInt64();
    run.config_json = stmt.getColumn("config_json").getString();
    return run;
  });
}

Status RunStore::apply_transition(std::string_view run_id, std::string_view from_state,
                                  std::string_view to_state, std::string_view event_json,
                                  const std::vector<NewOperation>& intents, UnixTime now) {
  return sql_guard("apply_transition", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto applied = apply_transition_in_tx(impl_->db, run_id, from_state, to_state,
                                          event_json, intents, now);
    if (!applied) {
      return applied;
    }
    txn.commit();
    return {};
  });
}

Result<std::vector<TransitionRecord>> RunStore::list_transitions(
    std::string_view run_id, std::size_t limit) const {
  return sql_guard("list_transitions", [&]() -> Result<std::vector<TransitionRecord>> {
    std::string sql =
        "SELECT id, run_id, from_state, to_state, event_json, at FROM transitions "
        "WHERE run_id = ? ORDER BY id";
    if (limit > 0) {
      sql = "SELECT id, run_id, from_state, to_state, event_json, at FROM transitions "
            "WHERE run_id = ? ORDER BY id DESC LIMIT ?";
    }
    SQLite::Statement stmt(impl_->db, sql);
    stmt.bind(1, std::string(run_id));
    if (limit > 0) {
      stmt.bind(2, static_cast<int64_t>(limit));
    }
    std::vector<TransitionRecord> out;
    while (stmt.executeStep()) {
      TransitionRecord record;
      record.id = stmt.getColumn("id").getInt64();
      record.run_id = stmt.getColumn("run_id").getString();
      record.from_state = stmt.getColumn("from_state").getString();
      record.to_state = stmt.getColumn("to_state").getString();
      record.event_json = stmt.getColumn("event_json").getString();
      record.at = stmt.getColumn("at").getInt64();
      out.push_back(std::move(record));
    }
    if (limit > 0) {
      std::reverse(out.begin(), out.end());  // chronological order
    }
    return out;
  });
}

Status RunStore::upsert_milestone(const MilestoneRecord& milestone) {
  return sql_guard("upsert_milestone", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO milestones (run_id, milestone_id, state, iterations) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(run_id, milestone_id) DO UPDATE SET state = excluded.state, "
        "iterations = excluded.iterations");
    stmt.bind(1, milestone.run_id);
    stmt.bind(2, milestone.milestone_id);
    stmt.bind(3, milestone.state);
    stmt.bind(4, static_cast<int>(milestone.iterations));
    stmt.exec();
    return {};
  });
}

Result<MilestoneRecord> RunStore::get_milestone(std::string_view run_id,
                                                std::string_view milestone_id) const {
  return sql_guard("get_milestone", [&]() -> Result<MilestoneRecord> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT run_id, milestone_id, state, iterations FROM milestones "
        "WHERE run_id = ? AND milestone_id = ?");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, std::string(milestone_id));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound,
                  "milestone not found: " + std::string(run_id) + "/" +
                      std::string(milestone_id));
    }
    MilestoneRecord milestone;
    milestone.run_id = stmt.getColumn("run_id").getString();
    milestone.milestone_id = stmt.getColumn("milestone_id").getString();
    milestone.state = stmt.getColumn("state").getString();
    milestone.iterations =
        static_cast<uint32_t>(stmt.getColumn("iterations").getInt());
    return milestone;
  });
}

Result<std::vector<MilestoneRecord>> RunStore::list_milestones(
    std::string_view run_id) const {
  return sql_guard("list_milestones", [&]() -> Result<std::vector<MilestoneRecord>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT run_id, milestone_id, state, iterations FROM milestones "
        "WHERE run_id = ? ORDER BY milestone_id");
    stmt.bind(1, std::string(run_id));
    std::vector<MilestoneRecord> out;
    while (stmt.executeStep()) {
      MilestoneRecord milestone;
      milestone.run_id = stmt.getColumn("run_id").getString();
      milestone.milestone_id = stmt.getColumn("milestone_id").getString();
      milestone.state = stmt.getColumn("state").getString();
      milestone.iterations =
          static_cast<uint32_t>(stmt.getColumn("iterations").getInt());
      out.push_back(std::move(milestone));
    }
    return out;
  });
}

Status RunStore::set_milestone_state(std::string_view run_id,
                                     std::string_view milestone_id,
                                     std::string_view state, uint32_t iterations,
                                     UnixTime now) {
  (void)now;  // milestone table has no timestamp column in the design schema
  return sql_guard("set_milestone_state", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "UPDATE milestones SET state = ?, iterations = ? "
        "WHERE run_id = ? AND milestone_id = ?");
    stmt.bind(1, std::string(state));
    stmt.bind(2, static_cast<int>(iterations));
    stmt.bind(3, std::string(run_id));
    stmt.bind(4, std::string(milestone_id));
    if (stmt.exec() == 0) {
      return fail(error_codes::kNotFound,
                  "milestone not found: " + std::string(run_id) + "/" +
                      std::string(milestone_id));
    }
    return {};
  });
}

Status RunStore::record_intents(std::string_view run_id,
                                const std::vector<NewOperation>& intents, UnixTime now) {
  return sql_guard("record_intents", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    for (const auto& intent : intents) {
      auto inserted = insert_operation(impl_->db, run_id, intent, now);
      if (!inserted) {
        return inserted;
      }
    }
    txn.commit();
    return {};
  });
}

Result<OperationRecord> RunStore::get_operation(std::string_view op_id) const {
  return sql_guard("get_operation",
                   [&]() -> Result<OperationRecord> { return load_operation(impl_->db, op_id); });
}

Result<OperationRecord> RunStore::find_operation_by_key(
    std::string_view idempotency_key) const {
  return sql_guard("find_operation_by_key", [&]() -> Result<OperationRecord> {
    SQLite::Statement stmt(
        impl_->db, std::string("SELECT ") + kOperationColumns +
                       " FROM operations WHERE idempotency_key = ?");
    stmt.bind(1, std::string(idempotency_key));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound,
                  "no operation with idempotency key " + std::string(idempotency_key));
    }
    return read_operation(stmt);
  });
}

Result<std::vector<OperationRecord>> RunStore::list_operations(
    std::string_view run_id, const std::vector<std::string>& states) const {
  return sql_guard("list_operations", [&]() -> Result<std::vector<OperationRecord>> {
    std::string sql = std::string("SELECT ") + kOperationColumns +
                      " FROM operations WHERE run_id = ?";
    if (!states.empty()) {
      sql += " AND state IN (";
      for (std::size_t i = 0; i < states.size(); ++i) {
        sql += i == 0 ? "?" : ", ?";
      }
      sql += ")";
    }
    sql += " ORDER BY created_at, op_id";
    SQLite::Statement stmt(impl_->db, sql);
    stmt.bind(1, std::string(run_id));
    for (std::size_t i = 0; i < states.size(); ++i) {
      stmt.bind(static_cast<int>(i) + 2, states[i]);
    }
    std::vector<OperationRecord> out;
    while (stmt.executeStep()) {
      out.push_back(read_operation(stmt));
    }
    return out;
  });
}

Status RunStore::mark_operation_started(std::string_view op_id, uint64_t epoch,
                                        UnixTime now) {
  return sql_guard("mark_operation_started", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto op = load_operation(impl_->db, op_id);
    if (!op) {
      return tl::make_unexpected(op.error());
    }
    if (op->state != op_state::kPending) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " is " + op->state +
                      ", expected PENDING");
    }
    auto fenced = check_epoch(impl_->db, op->run_id, epoch);
    if (!fenced) {
      return fenced;
    }
    SQLite::Statement stmt(
        impl_->db,
        "UPDATE operations SET state = ?, attempts = attempts + 1, epoch = ?, "
        "updated_at = ? WHERE op_id = ? AND state = ?");
    stmt.bind(1, op_state::kStarted);
    stmt.bind(2, static_cast<int64_t>(epoch));
    stmt.bind(3, static_cast<int64_t>(now));
    stmt.bind(4, std::string(op_id));
    stmt.bind(5, op_state::kPending);
    if (stmt.exec() == 0) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " is not PENDING");
    }
    txn.commit();
    return {};
  });
}

Status RunStore::confirm_operation(std::string_view op_id, std::string_view result_json,
                                   uint64_t epoch, UnixTime now) {
  return sql_guard("confirm_operation", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto op = load_operation(impl_->db, op_id);
    if (!op) {
      return tl::make_unexpected(op.error());
    }
    auto fenced = check_epoch(impl_->db, op->run_id, epoch);
    if (!fenced) {
      return fenced;
    }
    if (op->state != op_state::kStarted && op->state != op_state::kNeedsReconcile) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " is " + op->state +
                      ", expected STARTED or NEEDS_RECONCILE");
    }
    auto updated = update_operation_state(impl_->db, op_id, op->state,
                                          op_state::kConfirmed, result_json, now);
    if (!updated) {
      return updated;
    }
    txn.commit();
    return {};
  });
}

Status RunStore::confirm_operation_with_transition(
    std::string_view op_id, std::string_view result_json, uint64_t epoch,
    std::string_view run_id, std::string_view from_state, std::string_view to_state,
    std::string_view event_json, const std::vector<NewOperation>& intents, UnixTime now) {
  return sql_guard("confirm_operation_with_transition", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto op = load_operation(impl_->db, op_id);
    if (!op) {
      return tl::make_unexpected(op.error());
    }
    if (op->run_id != run_id) {
      return fail(error_codes::kConflict,
                  "operation " + std::string(op_id) + " belongs to run " + op->run_id);
    }
    auto fenced = check_epoch(impl_->db, op->run_id, epoch);
    if (!fenced) {
      return fenced;
    }
    auto applied = apply_transition_in_tx(impl_->db, run_id, from_state, to_state,
                                          event_json, intents, now);
    if (!applied) {
      return applied;
    }
    auto updated = update_operation_state(impl_->db, op_id, op_state::kStarted,
                                          op_state::kConfirmed, result_json, now);
    if (!updated) {
      return updated;
    }
    txn.commit();
    return {};
  });
}

Status RunStore::fail_operation(std::string_view op_id, std::string_view error_json,
                                UnixTime now) {
  return sql_guard("fail_operation", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto op = load_operation(impl_->db, op_id);
    if (!op) {
      return tl::make_unexpected(op.error());
    }
    if (op->state != op_state::kStarted && op->state != op_state::kPending &&
        op->state != op_state::kNeedsReconcile) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " is " + op->state +
                      ", expected STARTED, PENDING or NEEDS_RECONCILE");
    }
    auto updated = update_operation_state(impl_->db, op_id, op->state,
                                          op_state::kFailed, error_json, now);
    if (!updated) {
      return updated;
    }
    txn.commit();
    return {};
  });
}

Status RunStore::mark_operation_needs_reconcile(std::string_view op_id,
                                                std::string_view note, UnixTime now) {
  return sql_guard("mark_operation_needs_reconcile", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto op = load_operation(impl_->db, op_id);
    if (!op) {
      return tl::make_unexpected(op.error());
    }
    if (op->state != op_state::kStarted && op->state != op_state::kPending &&
        op->state != op_state::kNeedsReconcile) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " is " + op->state +
                      ", expected STARTED, PENDING or NEEDS_RECONCILE");
    }
    auto updated = update_operation_state(impl_->db, op_id, op->state,
                                          op_state::kNeedsReconcile, note, now);
    if (!updated) {
      return updated;
    }
    txn.commit();
    return {};
  });
}

Status RunStore::retry_operation(std::string_view op_id, UnixTime now) {
  return sql_guard("retry_operation", [&]() -> Status {
    SQLite::Transaction txn(impl_->db);
    auto op = load_operation(impl_->db, op_id);
    if (!op) {
      return tl::make_unexpected(op.error());
    }
    if (op->state != op_state::kStarted && op->state != op_state::kNeedsReconcile) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " is " + op->state +
                      ", expected STARTED or NEEDS_RECONCILE");
    }
    SQLite::Statement stmt(
        impl_->db,
        "UPDATE operations SET state = ?, updated_at = ? WHERE op_id = ? AND state = ?");
    stmt.bind(1, op_state::kPending);
    stmt.bind(2, static_cast<int64_t>(now));
    stmt.bind(3, std::string(op_id));
    stmt.bind(4, op->state);
    if (stmt.exec() == 0) {
      return fail(error_codes::kInvalidOperationState,
                  "operation " + std::string(op_id) + " changed concurrently");
    }
    txn.commit();
    return {};
  });
}

Status RunStore::record_gate_result(const GateResultRow& row) {
  return sql_guard("record_gate_result", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO gate_results (run_id, milestone_id, commit_sha, gate, passed, "
        "report_ref, at) VALUES (?, ?, ?, ?, ?, ?, ?)");
    stmt.bind(1, row.run_id);
    stmt.bind(2, row.milestone_id);
    stmt.bind(3, row.commit_sha);
    stmt.bind(4, row.gate);
    stmt.bind(5, row.passed ? 1 : 0);
    stmt.bind(6, row.report_ref);
    stmt.bind(7, static_cast<int64_t>(row.at));
    stmt.exec();
    return {};
  });
}

Result<std::vector<GateResultRow>> RunStore::list_gate_results(
    std::string_view run_id, std::string_view commit_sha) const {
  return sql_guard("list_gate_results", [&]() -> Result<std::vector<GateResultRow>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT id, run_id, milestone_id, commit_sha, gate, passed, report_ref, at "
        "FROM gate_results WHERE run_id = ? AND commit_sha = ? ORDER BY id");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, std::string(commit_sha));
    std::vector<GateResultRow> out;
    while (stmt.executeStep()) {
      GateResultRow row;
      row.id = stmt.getColumn("id").getInt64();
      row.run_id = stmt.getColumn("run_id").getString();
      row.milestone_id = stmt.getColumn("milestone_id").getString();
      row.commit_sha = stmt.getColumn("commit_sha").getString();
      row.gate = stmt.getColumn("gate").getString();
      row.passed = stmt.getColumn("passed").getInt() != 0;
      row.report_ref = stmt.getColumn("report_ref").getString();
      row.at = stmt.getColumn("at").getInt64();
      out.push_back(std::move(row));
    }
    return out;
  });
}

Result<bool> RunStore::all_gates_passed(std::string_view run_id,
                                        std::string_view commit_sha) const {
  return sql_guard("all_gates_passed", [&]() -> Result<bool> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT COUNT(*), COALESCE(SUM(passed), 0) FROM gate_results "
        "WHERE run_id = ? AND commit_sha = ?");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, std::string(commit_sha));
    stmt.executeStep();
    const int total = stmt.getColumn(0).getInt();
    const int passed = stmt.getColumn(1).getInt();
    return total > 0 && total == passed;
  });
}

Result<std::vector<GateResultRow>> RunStore::latest_gate_results(
    std::string_view run_id, std::string_view milestone_id) const {
  return sql_guard("latest_gate_results", [&]() -> Result<std::vector<GateResultRow>> {
    std::string sql = "SELECT commit_sha FROM gate_results WHERE run_id = ?";
    if (!milestone_id.empty()) {
      sql += " AND milestone_id = ?";
    }
    sql += " ORDER BY id DESC LIMIT 1";
    SQLite::Statement stmt(impl_->db, sql);
    stmt.bind(1, std::string(run_id));
    if (!milestone_id.empty()) {
      stmt.bind(2, std::string(milestone_id));
    }
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound,
                  "no gate results for run " + std::string(run_id));
    }
    return list_gate_results(run_id, stmt.getColumn(0).getString());
  });
}

Status RunStore::upsert_defect(const DefectRecord& defect) {
  return sql_guard("upsert_defect", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO defects (id, run_id, milestone_id, status, severity, fingerprint, "
        "body_json, attempts) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET run_id = excluded.run_id, "
        "milestone_id = excluded.milestone_id, status = excluded.status, "
        "severity = excluded.severity, fingerprint = excluded.fingerprint, "
        "body_json = excluded.body_json, attempts = excluded.attempts");
    stmt.bind(1, defect.id);
    stmt.bind(2, defect.run_id);
    stmt.bind(3, defect.milestone_id);
    stmt.bind(4, defect.status);
    stmt.bind(5, defect.severity);
    stmt.bind(6, defect.fingerprint);
    stmt.bind(7, defect.body_json);
    stmt.bind(8, static_cast<int>(defect.attempts));
    stmt.exec();
    return {};
  });
}

namespace {

DefectRecord read_defect(const SQLite::Statement& stmt) {
  DefectRecord defect;
  defect.id = stmt.getColumn("id").getString();
  defect.run_id = stmt.getColumn("run_id").getString();
  defect.milestone_id = stmt.getColumn("milestone_id").getString();
  defect.status = stmt.getColumn("status").getString();
  defect.severity = stmt.getColumn("severity").getString();
  defect.fingerprint = stmt.getColumn("fingerprint").getString();
  defect.body_json = stmt.getColumn("body_json").getString();
  defect.attempts = static_cast<uint32_t>(stmt.getColumn("attempts").getInt());
  return defect;
}

}  // namespace

Result<DefectRecord> RunStore::get_defect(std::string_view id) const {
  return sql_guard("get_defect", [&]() -> Result<DefectRecord> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT id, run_id, milestone_id, status, severity, fingerprint, body_json, "
        "attempts FROM defects WHERE id = ?");
    stmt.bind(1, std::string(id));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound, "defect not found: " + std::string(id));
    }
    return read_defect(stmt);
  });
}

Result<DefectRecord> RunStore::find_defect_by_fingerprint(
    std::string_view run_id, std::string_view fingerprint) const {
  return sql_guard("find_defect_by_fingerprint", [&]() -> Result<DefectRecord> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT id, run_id, milestone_id, status, severity, fingerprint, body_json, "
        "attempts FROM defects WHERE run_id = ? AND fingerprint = ? LIMIT 1");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, std::string(fingerprint));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound,
                  "no defect with fingerprint " + std::string(fingerprint));
    }
    return read_defect(stmt);
  });
}

Result<std::vector<DefectRecord>> RunStore::list_defects(
    std::string_view run_id, std::string_view milestone_id,
    const std::vector<std::string>& statuses) const {
  return sql_guard("list_defects", [&]() -> Result<std::vector<DefectRecord>> {
    std::string sql =
        "SELECT id, run_id, milestone_id, status, severity, fingerprint, body_json, "
        "attempts FROM defects WHERE run_id = ?";
    if (!milestone_id.empty()) {
      sql += " AND milestone_id = ?";
    }
    if (!statuses.empty()) {
      sql += " AND status IN (";
      for (std::size_t i = 0; i < statuses.size(); ++i) {
        sql += i == 0 ? "?" : ", ?";
      }
      sql += ")";
    }
    sql += " ORDER BY id";
    SQLite::Statement stmt(impl_->db, sql);
    int bind_index = 1;
    stmt.bind(bind_index++, std::string(run_id));
    if (!milestone_id.empty()) {
      stmt.bind(bind_index++, std::string(milestone_id));
    }
    for (const auto& status : statuses) {
      stmt.bind(bind_index++, status);
    }
    std::vector<DefectRecord> out;
    while (stmt.executeStep()) {
      out.push_back(read_defect(stmt));
    }
    return out;
  });
}

Status RunStore::set_defect_status(std::string_view id, std::string_view status,
                                   uint32_t attempts) {
  return sql_guard("set_defect_status", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db, "UPDATE defects SET status = ?, attempts = ? WHERE id = ?");
    stmt.bind(1, std::string(status));
    stmt.bind(2, static_cast<int>(attempts));
    stmt.bind(3, std::string(id));
    if (stmt.exec() == 0) {
      return fail(error_codes::kNotFound, "defect not found: " + std::string(id));
    }
    return {};
  });
}

Status RunStore::save_checkpoint(const CheckpointRecord& checkpoint) {
  return sql_guard("save_checkpoint", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO checkpoints (run_id, milestone_id, worktree_heads_json, "
        "sessions_json, milestone_state, at) VALUES (?, ?, ?, ?, ?, ?)");
    stmt.bind(1, checkpoint.run_id);
    stmt.bind(2, checkpoint.milestone_id);
    stmt.bind(3, checkpoint.worktree_heads_json);
    stmt.bind(4, checkpoint.sessions_json);
    stmt.bind(5, checkpoint.milestone_state);
    stmt.bind(6, static_cast<int64_t>(checkpoint.at));
    stmt.exec();
    return {};
  });
}

Result<CheckpointRecord> RunStore::latest_checkpoint(
    std::string_view run_id, std::string_view milestone_id) const {
  return sql_guard("latest_checkpoint", [&]() -> Result<CheckpointRecord> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT id, run_id, milestone_id, worktree_heads_json, sessions_json, "
        "milestone_state, at FROM checkpoints WHERE run_id = ? AND milestone_id = ? "
        "ORDER BY at DESC, id DESC LIMIT 1");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, std::string(milestone_id));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound,
                  "no checkpoint for " + std::string(run_id) + "/" +
                      std::string(milestone_id));
    }
    CheckpointRecord checkpoint;
    checkpoint.id = stmt.getColumn("id").getInt64();
    checkpoint.run_id = stmt.getColumn("run_id").getString();
    checkpoint.milestone_id = stmt.getColumn("milestone_id").getString();
    checkpoint.worktree_heads_json = stmt.getColumn("worktree_heads_json").getString();
    checkpoint.sessions_json = stmt.getColumn("sessions_json").getString();
    checkpoint.milestone_state = stmt.getColumn("milestone_state").getString();
    checkpoint.at = stmt.getColumn("at").getInt64();
    return checkpoint;
  });
}

Result<LockRecord> RunStore::acquire_lock(std::string_view lock_id, std::string_view run_id,
                                        std::string_view kind,
                                        std::string_view holder_milestone, uint64_t epoch,
                                        UnixTime now) {
  return sql_guard("acquire_lock", [&]() -> Result<LockRecord> {
    SQLite::Transaction txn(impl_->db);
    SQLite::Statement select(
        impl_->db,
        "SELECT lock_id, run_id, kind, holder_milestone, epoch, acquired_at "
        "FROM locks WHERE lock_id = ?");
    select.bind(1, std::string(lock_id));
    if (select.executeStep()) {
      const std::string holder = select.getColumn("holder_milestone").getString();
      const uint64_t held_epoch =
          static_cast<uint64_t>(select.getColumn("epoch").getInt64());
      if (holder == holder_milestone && held_epoch == epoch) {
        LockRecord existing;
        existing.lock_id = select.getColumn("lock_id").getString();
        existing.run_id = select.getColumn("run_id").getString();
        existing.kind = select.getColumn("kind").getString();
        existing.holder_milestone = holder;
        existing.epoch = held_epoch;
        existing.acquired_at = select.getColumn("acquired_at").getInt64();
        txn.commit();
        return existing;
      }
      return fail(error_codes::kLockHeld,
                  "lock " + std::string(lock_id) + " is held by milestone " + holder +
                      " (epoch " + std::to_string(held_epoch) + ")");
    }
    SQLite::Statement insert(
        impl_->db,
        "INSERT INTO locks (lock_id, run_id, kind, holder_milestone, epoch, acquired_at) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    insert.bind(1, std::string(lock_id));
    insert.bind(2, std::string(run_id));
    insert.bind(3, std::string(kind));
    insert.bind(4, std::string(holder_milestone));
    insert.bind(5, static_cast<int64_t>(epoch));
    insert.bind(6, static_cast<int64_t>(now));
    insert.exec();
    txn.commit();
    return LockRecord{std::string(lock_id), std::string(run_id), std::string(kind),
                      std::string(holder_milestone), epoch, now};
  });
}

Status RunStore::release_lock(std::string_view lock_id, std::string_view holder_milestone,
                              uint64_t epoch) {
  return sql_guard("release_lock", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "DELETE FROM locks WHERE lock_id = ? AND holder_milestone = ? AND epoch = ?");
    stmt.bind(1, std::string(lock_id));
    stmt.bind(2, std::string(holder_milestone));
    stmt.bind(3, static_cast<int64_t>(epoch));
    if (stmt.exec() == 0) {
      return fail(error_codes::kLockNotFound,
                  "lock " + std::string(lock_id) + " is not held by milestone " +
                      std::string(holder_milestone) + " at epoch " +
                      std::to_string(epoch));
    }
    return {};
  });
}

Result<LockRecord> RunStore::get_lock(std::string_view lock_id) const {
  return sql_guard("get_lock", [&]() -> Result<LockRecord> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT lock_id, run_id, kind, holder_milestone, epoch, acquired_at "
        "FROM locks WHERE lock_id = ?");
    stmt.bind(1, std::string(lock_id));
    if (!stmt.executeStep()) {
      return fail(error_codes::kLockNotFound, "no lock with id " + std::string(lock_id));
    }
    LockRecord lock;
    lock.lock_id = stmt.getColumn("lock_id").getString();
    lock.run_id = stmt.getColumn("run_id").getString();
    lock.kind = stmt.getColumn("kind").getString();
    lock.holder_milestone = stmt.getColumn("holder_milestone").getString();
    lock.epoch = static_cast<uint64_t>(stmt.getColumn("epoch").getInt64());
    lock.acquired_at = stmt.getColumn("acquired_at").getInt64();
    return lock;
  });
}

Result<std::vector<LockRecord>> RunStore::list_locks(std::string_view run_id) const {
  return sql_guard("list_locks", [&]() -> Result<std::vector<LockRecord>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT lock_id, run_id, kind, holder_milestone, epoch, acquired_at "
        "FROM locks WHERE run_id = ? ORDER BY lock_id");
    stmt.bind(1, std::string(run_id));
    std::vector<LockRecord> out;
    while (stmt.executeStep()) {
      LockRecord lock;
      lock.lock_id = stmt.getColumn("lock_id").getString();
      lock.run_id = stmt.getColumn("run_id").getString();
      lock.kind = stmt.getColumn("kind").getString();
      lock.holder_milestone = stmt.getColumn("holder_milestone").getString();
      lock.epoch = static_cast<uint64_t>(stmt.getColumn("epoch").getInt64());
      lock.acquired_at = stmt.getColumn("acquired_at").getInt64();
      out.push_back(std::move(lock));
    }
    return out;
  });
}

Result<std::size_t> RunStore::release_stale_locks(std::string_view run_id,
                                                  uint64_t current_epoch) {
  return sql_guard("release_stale_locks", [&]() -> Result<std::size_t> {
    SQLite::Statement stmt(
        impl_->db, "DELETE FROM locks WHERE run_id = ? AND epoch < ?");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, static_cast<int64_t>(current_epoch));
    return static_cast<std::size_t>(stmt.exec());
  });
}

Result<LeaseRecord> RunStore::acquire_lease(std::string_view run_id,
                                            std::string_view holder,
                                            std::chrono::seconds ttl, UnixTime now) {
  return sql_guard("acquire_lease", [&]() -> Result<LeaseRecord> {
    auto locked = impl_->ensure_lock();
    if (!locked) {
      return tl::make_unexpected(locked.error());
    }
    SQLite::Transaction txn(impl_->db);
    SQLite::Statement select(
        impl_->db, "SELECT holder, epoch, renewed_at FROM run_lease WHERE run_id = ?");
    select.bind(1, std::string(run_id));
    uint64_t epoch = 1;
    if (select.executeStep()) {
      const std::string current_holder = select.getColumn("holder").getString();
      const uint64_t current_epoch =
          static_cast<uint64_t>(select.getColumn("epoch").getInt64());
      const UnixTime renewed_at = select.getColumn("renewed_at").getInt64();
      // An empty holder marks a released lease; the row is kept so the epoch
      // stays monotonic across takeovers (design section 6.12.6).
      const bool live =
          !current_holder.empty() && renewed_at + ttl.count() > now;
      if (live && current_holder != holder) {
        return fail(error_codes::kLeaseHeld,
                    "run " + std::string(run_id) + " lease held by " + current_holder +
                        " (epoch " + std::to_string(current_epoch) + ")");
      }
      if (live && current_holder == holder) {
        txn.commit();
        return LeaseRecord{std::string(run_id), current_holder, current_epoch, renewed_at};
      }
      epoch = current_epoch + 1;
    }
    SQLite::Statement upsert(
        impl_->db,
        "INSERT INTO run_lease (run_id, holder, epoch, renewed_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(run_id) DO UPDATE SET holder = excluded.holder, "
        "epoch = excluded.epoch, renewed_at = excluded.renewed_at");
    upsert.bind(1, std::string(run_id));
    upsert.bind(2, std::string(holder));
    upsert.bind(3, static_cast<int64_t>(epoch));
    upsert.bind(4, static_cast<int64_t>(now));
    upsert.exec();
    txn.commit();
    return LeaseRecord{std::string(run_id), std::string(holder), epoch, now};
  });
}

Status RunStore::renew_lease(std::string_view run_id, std::string_view holder,
                             uint64_t epoch, UnixTime now) {
  return sql_guard("renew_lease", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "UPDATE run_lease SET renewed_at = ? WHERE run_id = ? AND holder = ? AND epoch = ?");
    stmt.bind(1, static_cast<int64_t>(now));
    stmt.bind(2, std::string(run_id));
    stmt.bind(3, std::string(holder));
    stmt.bind(4, static_cast<int64_t>(epoch));
    if (stmt.exec() == 0) {
      return fail(error_codes::kLeaseLost,
                  "run " + std::string(run_id) + " lease is not held by " +
                      std::string(holder) + " at epoch " + std::to_string(epoch));
    }
    return {};
  });
}

Result<LeaseRecord> RunStore::get_lease(std::string_view run_id) const {
  return sql_guard("get_lease", [&]() -> Result<LeaseRecord> {
    SQLite::Statement stmt(
        impl_->db, "SELECT run_id, holder, epoch, renewed_at FROM run_lease WHERE run_id = ?");
    stmt.bind(1, std::string(run_id));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound, "no lease for run " + std::string(run_id));
    }
    LeaseRecord lease;
    lease.run_id = stmt.getColumn("run_id").getString();
    lease.holder = stmt.getColumn("holder").getString();
    lease.epoch = static_cast<uint64_t>(stmt.getColumn("epoch").getInt64());
    lease.renewed_at = stmt.getColumn("renewed_at").getInt64();
    return lease;
  });
}

Status RunStore::release_lease(std::string_view run_id, std::string_view holder,
                               uint64_t epoch) {
  auto released = sql_guard("release_lease", [&]() -> Status {
    // The row is kept with an empty holder so the epoch stays monotonic; the
    // next acquisition continues from this epoch (design section 6.12.6).
    SQLite::Statement stmt(
        impl_->db,
        "UPDATE run_lease SET holder = '', renewed_at = 0 "
        "WHERE run_id = ? AND holder = ? AND epoch = ?");
    stmt.bind(1, std::string(run_id));
    stmt.bind(2, std::string(holder));
    stmt.bind(3, static_cast<int64_t>(epoch));
    if (stmt.exec() == 0) {
      return fail(error_codes::kLeaseLost,
                  "run " + std::string(run_id) + " lease is not held by " +
                      std::string(holder) + " at epoch " + std::to_string(epoch));
    }
    return {};
  });
  if (!released) {
    return released;
  }
  if (impl_->lock_fd >= 0) {
    ::flock(impl_->lock_fd, LOCK_UN);
    ::close(impl_->lock_fd);
    impl_->lock_fd = -1;
  }
  return {};
}

Status RunStore::record_llm_call(const LlmCallRecord& call) {
  return sql_guard("record_llm_call", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO llm_calls (run_id, role, model, prompt_tokens, completion_tokens, "
        "cached_tokens, usd, latency_ms, prompt_ref, response_ref, at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    stmt.bind(1, call.run_id);
    stmt.bind(2, call.role);
    stmt.bind(3, call.model);
    stmt.bind(4, static_cast<int64_t>(call.prompt_tokens));
    stmt.bind(5, static_cast<int64_t>(call.completion_tokens));
    stmt.bind(6, static_cast<int64_t>(call.cached_tokens));
    stmt.bind(7, call.usd);
    stmt.bind(8, static_cast<int64_t>(call.latency_ms));
    stmt.bind(9, call.prompt_ref);
    stmt.bind(10, call.response_ref);
    stmt.bind(11, static_cast<int64_t>(call.at));
    stmt.exec();
    return {};
  });
}

Result<double> RunStore::total_usd(std::string_view run_id) const {
  return sql_guard("total_usd", [&]() -> Result<double> {
    SQLite::Statement stmt(
        impl_->db, "SELECT COALESCE(SUM(usd), 0.0) FROM llm_calls WHERE run_id = ?");
    stmt.bind(1, std::string(run_id));
    stmt.executeStep();
    return stmt.getColumn(0).getDouble();
  });
}

Result<int64_t> RunStore::total_tokens(std::string_view run_id) const {
  return sql_guard("total_tokens", [&]() -> Result<int64_t> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT COALESCE(SUM(prompt_tokens + completion_tokens), 0) FROM llm_calls "
        "WHERE run_id = ?");
    stmt.bind(1, std::string(run_id));
    stmt.executeStep();
    return stmt.getColumn(0).getInt64();
  });
}

Status RunStore::upsert_doc_version(const DocVersionRow& row) {
  return sql_guard("upsert_doc_version", [&]() -> Status {
    SQLite::Statement stmt(
        impl_->db,
        "INSERT INTO doc_versions (doc_id, version, sha256, precedence, ingested_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(doc_id, version) DO UPDATE SET sha256 = excluded.sha256, "
        "precedence = excluded.precedence, ingested_at = excluded.ingested_at");
    stmt.bind(1, row.doc_id);
    stmt.bind(2, row.version);
    stmt.bind(3, row.sha256);
    stmt.bind(4, static_cast<int>(row.precedence));
    stmt.bind(5, static_cast<int64_t>(row.ingested_at));
    stmt.exec();
    return {};
  });
}

Result<std::vector<DocVersionRow>> RunStore::list_doc_versions() const {
  return sql_guard("list_doc_versions", [&]() -> Result<std::vector<DocVersionRow>> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT doc_id, version, sha256, precedence, ingested_at FROM doc_versions "
        "ORDER BY doc_id, version");
    std::vector<DocVersionRow> out;
    while (stmt.executeStep()) {
      DocVersionRow row;
      row.doc_id = stmt.getColumn("doc_id").getString();
      row.version = stmt.getColumn("version").getString();
      row.sha256 = stmt.getColumn("sha256").getString();
      row.precedence = stmt.getColumn("precedence").getInt();
      row.ingested_at = stmt.getColumn("ingested_at").getInt64();
      out.push_back(std::move(row));
    }
    return out;
  });
}

Result<DocVersionRow> RunStore::get_doc_version(std::string_view doc_id,
                                                std::string_view version) const {
  return sql_guard("get_doc_version", [&]() -> Result<DocVersionRow> {
    SQLite::Statement stmt(
        impl_->db,
        "SELECT doc_id, version, sha256, precedence, ingested_at FROM doc_versions "
        "WHERE doc_id = ? AND version = ?");
    stmt.bind(1, std::string(doc_id));
    stmt.bind(2, std::string(version));
    if (!stmt.executeStep()) {
      return fail(error_codes::kNotFound,
                  "document version not found: " + std::string(doc_id) + "@" +
                      std::string(version));
    }
    DocVersionRow row;
    row.doc_id = stmt.getColumn("doc_id").getString();
    row.version = stmt.getColumn("version").getString();
    row.sha256 = stmt.getColumn("sha256").getString();
    row.precedence = stmt.getColumn("precedence").getInt();
    row.ingested_at = stmt.getColumn("ingested_at").getInt64();
    return row;
  });
}

}  // namespace cf::store
