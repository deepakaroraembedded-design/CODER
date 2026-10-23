// cf-orch: side-effect dispatcher skeleton (design section 6.12.2).
//
// The dispatcher walks the intent-and-reconcile protocol: it only performs an
// operation that already has a committed PENDING intent, stamps every attempt
// with the run lease epoch, and refuses to act on operations from an older
// epoch (design section 6.12.6). Handlers are the pluggable external effects
// (git, GitHub, opencode turns, ...); they must be safe to call again after a
// crash, either idempotent or able to detect that the effect already happened.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "cf/common/result.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

namespace error_codes {
inline constexpr const char* kNoHandler = "DISPATCH_NO_HANDLER";
}  // namespace error_codes

enum class DispatchOutcome {
  Confirmed,       // the effect happened (now, or in a previous attempt)
  Retry,           // safe to retry under the current epoch
  Failed,          // permanent failure; escalation path
  NeedsReconcile,  // the world is in a state CF did not expect; escalate
};

struct DispatchResult {
  DispatchOutcome outcome = DispatchOutcome::Retry;
  std::string result_json;  // recorded on CONFIRMED
  std::string detail;       // recorded on FAILED / NEEDS_RECONCILE
};

class OperationHandler {
 public:
  virtual ~OperationHandler() = default;

  // The operation kind this handler serves (matches operations.kind).
  virtual std::string_view kind() const = 0;

  // Performs the external effect for a STARTED operation.
  virtual DispatchResult perform(const store::OperationRecord& op) = 0;

  // Recovery hook (design section 6.12.4): decides what happened to an
  // operation left STARTED by an older epoch. The default asks for a retry.
  virtual DispatchResult reconcile(const store::OperationRecord& op);
};

class Dispatcher {
 public:
  Dispatcher(store::RunStore& store, std::string run_id, std::string holder);

  void register_handler(std::unique_ptr<OperationHandler> handler);

  // Marks every PENDING operation of the run STARTED under `epoch` and
  // performs it, applying the handler's outcome. Returns the number of
  // operations dispatched.
  Result<std::size_t> dispatch_pending(uint64_t epoch, store::UnixTime now);

  // After a takeover, reconciles operations left STARTED by an older epoch,
  // once the in-flight grace period has passed (design section 6.12.6 rule
  // 3). Operations started under `epoch` or younger than the grace period
  // are left alone.
  Result<std::size_t> reconcile_stale(uint64_t epoch, std::chrono::seconds grace,
                                      store::UnixTime now);

  // Reconciles one operation through its handler (design section 6.12.4 step
  // 3); used by recovery for operations in NEEDS_RECONCILE. An operation with
  // no registered handler stays NEEDS_RECONCILE.
  Result<DispatchOutcome> reconcile_one(std::string_view op_id, uint64_t epoch,
                                        store::UnixTime now);

  // Reconciles every operation in NEEDS_RECONCILE (design section 6.12.4 step
  // 3). Returns how many operations were offered to their reconcilers; those
  // whose reconciler still answers NeedsReconcile stay in that state.
  Result<std::size_t> reconcile_needs_reconcile(uint64_t epoch, store::UnixTime now);

 private:
  Status apply_outcome(std::string_view op_id, uint64_t epoch,
                       const DispatchResult& result, store::UnixTime now);

  store::RunStore& store_;
  std::string run_id_;
  std::string holder_;
  std::map<std::string, std::unique_ptr<OperationHandler>, std::less<>> handlers_;
};

}  // namespace cf::orch
