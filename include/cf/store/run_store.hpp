// cf-store: the run store (design sections 6.12, 8.4).
//
// Transactional SQLite (WAL mode) persistence for run and milestone state,
// the operation intent log of the side-effect protocol, checkpoints, the run
// lease, and the LLM call ledger. The store is the source of truth: no
// external effect may be performed without a committed PENDING intent
// (design section 6.12.2, rule 4).
//
// The store is intended to be used by one thread (the orchestrator); SQLite
// itself is opened in its default serialized mode.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cf/common/result.hpp"

namespace cf::store {

using UnixTime = int64_t;

// Wall-clock seconds since the Unix epoch.
UnixTime unix_now();

// Fresh operation id ("op_" + 16 hex chars), used when a caller does not
// supply one.
std::string new_op_id();

// Stable error codes (design section 4.1: one registry).
namespace error_codes {
inline constexpr const char* kOpenFailed = "STORE_OPEN_FAILED";
inline constexpr const char* kQueryFailed = "STORE_QUERY_FAILED";
inline constexpr const char* kNotFound = "STORE_NOT_FOUND";
inline constexpr const char* kConflict = "STORE_CONFLICT";
inline constexpr const char* kFileLockFailed = "STORE_FILE_LOCK_FAILED";
inline constexpr const char* kLeaseHeld = "STORE_LEASE_HELD";
inline constexpr const char* kLeaseLost = "STORE_LEASE_LOST";
inline constexpr const char* kStaleEpoch = "STORE_STALE_EPOCH";
inline constexpr const char* kInvalidTransition = "STORE_INVALID_TRANSITION";
inline constexpr const char* kInvalidOperationState = "STORE_INVALID_OPERATION_STATE";
inline constexpr const char* kLockHeld = "STORE_LOCK_HELD";
inline constexpr const char* kLockNotFound = "STORE_LOCK_NOT_FOUND";
}  // namespace error_codes

// Lock kinds (design section 6.13.2): one integration lock per run, one
// interface lock per public interface.
namespace lock_kind {
inline constexpr const char* kIntegration = "integration";
inline constexpr const char* kInterface = "interface";
}  // namespace lock_kind

inline std::string integration_lock_id() { return "integration"; }

inline std::string interface_lock_id(std::string_view interface_name) {
  return "interface:" + std::string(interface_name);
}

// Operation states (design section 6.12.2).
namespace op_state {
inline constexpr const char* kPending = "PENDING";
inline constexpr const char* kStarted = "STARTED";
inline constexpr const char* kConfirmed = "CONFIRMED";
inline constexpr const char* kFailed = "FAILED";
inline constexpr const char* kNeedsReconcile = "NEEDS_RECONCILE";
}  // namespace op_state

struct RunRecord {
  std::string run_id;
  std::string plan_id;
  std::string state;
  std::string config_json;
  UnixTime created_at = 0;
};

struct MilestoneRecord {
  std::string run_id;
  std::string milestone_id;
  std::string state;
  uint32_t iterations = 0;
};

struct OperationRecord {
  std::string op_id;
  std::string idempotency_key;
  std::string run_id;
  std::string milestone_id;
  std::string kind;
  std::string state;
  std::string input_json;
  std::string expected_json;
  std::string result_json;
  uint32_t attempts = 0;
  uint64_t epoch = 0;
  UnixTime created_at = 0;
  UnixTime updated_at = 0;
};

// An intent to be recorded with a state transition, in the same transaction
// (design section 6.12.2 step 1).
struct NewOperation {
  std::string op_id;  // empty: generated
  std::string idempotency_key;
  std::string milestone_id;
  std::string kind;
  std::string input_json;
  std::string expected_json;
};

struct CheckpointRecord {
  int64_t id = 0;
  std::string run_id;
  std::string milestone_id;
  std::string worktree_heads_json;
  std::string sessions_json;
  std::string milestone_state;
  UnixTime at = 0;
};

struct LeaseRecord {
  std::string run_id;
  std::string holder;
  uint64_t epoch = 0;
  UnixTime renewed_at = 0;
};

struct LlmCallRecord {
  std::string run_id;
  std::string role;
  std::string model;
  int64_t prompt_tokens = 0;
  int64_t completion_tokens = 0;
  int64_t cached_tokens = 0;
  double usd = 0.0;
  int64_t latency_ms = 0;
  std::string prompt_ref;
  std::string response_ref;
  UnixTime at = 0;
};

// One ingested document version (design section 8.4, doc_versions table).
struct DocVersionRow {
  std::string doc_id;
  std::string version;
  std::string sha256;
  int precedence = 0;
  UnixTime ingested_at = 0;
};

struct LockRecord {
  std::string lock_id;
  std::string run_id;
  std::string kind;  // lock_kind::kIntegration or kInterface
  std::string holder_milestone;
  uint64_t epoch = 0;
  UnixTime acquired_at = 0;
};

struct GateResultRow {
  int64_t id = 0;
  std::string run_id;
  std::string milestone_id;
  std::string commit_sha;
  std::string gate;
  bool passed = false;
  std::string report_ref;
  UnixTime at = 0;
};

struct TransitionRecord {
  int64_t id = 0;
  std::string run_id;
  std::string from_state;
  std::string to_state;
  std::string event_json;
  UnixTime at = 0;
};

struct DefectRecord {
  std::string id;
  std::string run_id;
  std::string milestone_id;
  std::string status;       // OPEN | IN_PROGRESS | FIXED_PENDING_VERIFY | VERIFIED |
                            // REOPENED | DISPUTED | WONT_FIX (design section 6.10)
  std::string severity;     // blocker | major | minor | high | medium | low
  std::string fingerprint;  // de-duplication across runs (design section 6.10)
  std::string body_json;    // the full defect payload
  uint32_t attempts = 0;
};

// The statuses that still need work (design section 11.1: a milestone cannot
// be Done while any of these are set).
inline std::vector<std::string> open_defect_statuses() {
  return {"OPEN", "IN_PROGRESS", "FIXED_PENDING_VERIFY", "REOPENED", "DISPUTED"};
}

class RunStore {
 public:
  // Opens (creating if needed) the store at db_path in WAL mode. The store
  // also owns the exclusive file lock `<db_path>.lease` used by the run
  // lease (design section 6.12.6).
  static Result<std::unique_ptr<RunStore>> open(const std::filesystem::path& db_path);

  ~RunStore();
  RunStore(const RunStore&) = delete;
  RunStore& operator=(const RunStore&) = delete;

  // --- runs and transitions (design sections 7.1, 8.4) ---------------------
  Status create_run(const RunRecord& run);
  Result<RunRecord> get_run(std::string_view run_id) const;
  // Replaces config_json; the run process records the resolved approval
  // deadlines here so later commands see the same values (design section
  // 12.1).
  Status update_run_config(std::string_view run_id, std::string_view config_json);

  // Applies one run state transition, appends the transition log row and
  // records any operation intents in a single transaction (design section
  // 6.12.2 step 1). The stored state must equal from_state; otherwise the
  // event is stale and STORE_INVALID_TRANSITION is returned.
  Status apply_transition(std::string_view run_id, std::string_view from_state,
                          std::string_view to_state, std::string_view event_json,
                          const std::vector<NewOperation>& intents, UnixTime now);

  // The run's transition log, oldest first. limit = 0 returns every row;
  // otherwise the newest `limit` rows are returned in chronological order.
  Result<std::vector<TransitionRecord>> list_transitions(std::string_view run_id,
                                                         std::size_t limit = 0) const;

  // --- milestones ----------------------------------------------------------
  Status upsert_milestone(const MilestoneRecord& milestone);
  Result<MilestoneRecord> get_milestone(std::string_view run_id,
                                        std::string_view milestone_id) const;
  Result<std::vector<MilestoneRecord>> list_milestones(std::string_view run_id) const;
  Status set_milestone_state(std::string_view run_id, std::string_view milestone_id,
                             std::string_view state, uint32_t iterations, UnixTime now);

  // --- operations (side-effect protocol, design section 6.12) --------------
  // Records intents without a state transition (the run state does not change
  // for every external effect); duplicates are rejected with STORE_CONFLICT.
  Status record_intents(std::string_view run_id,
                        const std::vector<NewOperation>& intents, UnixTime now);

  Result<OperationRecord> get_operation(std::string_view op_id) const;
  Result<OperationRecord> find_operation_by_key(std::string_view idempotency_key) const;
  Result<std::vector<OperationRecord>> list_operations(
      std::string_view run_id, const std::vector<std::string>& states) const;

  // PENDING -> STARTED; stamps the lease epoch that started it and counts the
  // attempt. Refused with STORE_STALE_EPOCH if epoch is not the current lease
  // epoch (design section 6.12.6).
  Status mark_operation_started(std::string_view op_id, uint64_t epoch, UnixTime now);

  // STARTED -> CONFIRMED with the actual result. Refused for a stale epoch.
  Status confirm_operation(std::string_view op_id, std::string_view result_json,
                           uint64_t epoch, UnixTime now);

  // The atomic form required by design section 6.12.2 step 3: confirm the
  // operation and apply the follow-on state transition (plus any new
  // intents) in one transaction.
  Status confirm_operation_with_transition(
      std::string_view op_id, std::string_view result_json, uint64_t epoch,
      std::string_view run_id, std::string_view from_state, std::string_view to_state,
      std::string_view event_json, const std::vector<NewOperation>& intents, UnixTime now);

  // Permanent failure: STARTED or PENDING -> FAILED.
  Status fail_operation(std::string_view op_id, std::string_view error_json, UnixTime now);

  // STARTED or PENDING -> NEEDS_RECONCILE, with a note for the escalation.
  Status mark_operation_needs_reconcile(std::string_view op_id, std::string_view note,
                                        UnixTime now);

  // Recovery: STARTED or NEEDS_RECONCILE -> PENDING so the dispatcher can
  // retry it under the current epoch (design section 6.12.4).
  Status retry_operation(std::string_view op_id, UnixTime now);

  // --- gate results (design sections 6.7, 10.4.6) --------------------------
  Status record_gate_result(const GateResultRow& row);

  Result<std::vector<GateResultRow>> list_gate_results(
      std::string_view run_id, std::string_view commit_sha) const;

  // True only when at least one gate result exists for the commit and every
  // one of them passed. Used by the pre-push check and the milestone Done
  // criteria (design section 11.1).
  Result<bool> all_gates_passed(std::string_view run_id,
                                std::string_view commit_sha) const;

  // The gate results of the newest commit that has any (optionally scoped to
  // a milestone); STORE_NOT_FOUND when no results exist.
  Result<std::vector<GateResultRow>> latest_gate_results(
      std::string_view run_id, std::string_view milestone_id) const;

  // --- defects (design sections 6.10, 8.3) ---------------------------------
  // Insert or update by id; the defect store de-duplicates by fingerprint
  // through find_defect_by_fingerprint before upserting.
  Status upsert_defect(const DefectRecord& defect);

  Result<DefectRecord> get_defect(std::string_view id) const;

  Result<DefectRecord> find_defect_by_fingerprint(std::string_view run_id,
                                                  std::string_view fingerprint) const;

  // Empty milestone_id = all milestones; empty statuses = all statuses.
  Result<std::vector<DefectRecord>> list_defects(
      std::string_view run_id, std::string_view milestone_id = "",
      const std::vector<std::string>& statuses = {}) const;

  Status set_defect_status(std::string_view id, std::string_view status,
                           uint32_t attempts);

  // --- checkpoints (design section 6.12.3) ---------------------------------
  Status save_checkpoint(const CheckpointRecord& checkpoint);
  Result<CheckpointRecord> latest_checkpoint(std::string_view run_id,
                                             std::string_view milestone_id) const;

  // --- locks (design section 6.13.2) ---------------------------------------
  // Locks have no TTL: they are held under the run lease and released by
  // recovery when their holder is gone (release_stale_locks). Re-acquiring a
  // lock you already hold is idempotent; a different holder is rejected.
  Result<LockRecord> acquire_lock(std::string_view lock_id, std::string_view run_id,
                                  std::string_view kind,
                                  std::string_view holder_milestone, uint64_t epoch,
                                  UnixTime now);

  Status release_lock(std::string_view lock_id, std::string_view holder_milestone,
                      uint64_t epoch);

  Result<LockRecord> get_lock(std::string_view lock_id) const;

  Result<std::vector<LockRecord>> list_locks(std::string_view run_id) const;

  // Releases every lock of the run acquired under an older lease epoch and
  // returns how many were released (design sections 6.12.4, 6.13.2).
  Result<std::size_t> release_stale_locks(std::string_view run_id, uint64_t current_epoch);

  // --- run lease (design section 6.12.6) -----------------------------------
  // Takes the exclusive file lock plus a lease row with an increasing epoch.
  // Fails with STORE_LEASE_HELD while another holder's lease is live
  // (renewed_at + ttl > now). Re-acquiring your own live lease returns it
  // unchanged.
  Result<LeaseRecord> acquire_lease(std::string_view run_id, std::string_view holder,
                                    std::chrono::seconds ttl, UnixTime now);

  Status renew_lease(std::string_view run_id, std::string_view holder, uint64_t epoch,
                     UnixTime now);

  Result<LeaseRecord> get_lease(std::string_view run_id) const;

  // Releases the lease and the file lock. The lease row is kept with an
  // empty holder so the epoch stays monotonic across acquisitions.
  Status release_lease(std::string_view run_id, std::string_view holder, uint64_t epoch);

  // --- LLM call ledger (design section 6.11) -------------------------------
  Status record_llm_call(const LlmCallRecord& call);
  Result<double> total_usd(std::string_view run_id) const;
  Result<int64_t> total_tokens(std::string_view run_id) const;

  // --- document versions (design section 8.4) ------------------------------
  // Records what was ingested (doc_id, version, content hash, precedence);
  // re-ingesting the same version replaces the row.
  Status upsert_doc_version(const DocVersionRow& row);

  Result<std::vector<DocVersionRow>> list_doc_versions() const;

  Result<DocVersionRow> get_doc_version(std::string_view doc_id,
                                        std::string_view version) const;

 private:
  struct Impl;
  explicit RunStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cf::store
