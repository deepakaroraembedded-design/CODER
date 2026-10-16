#include "cf/orch/dispatcher.hpp"

#include <utility>

namespace cf::orch {

DispatchResult OperationHandler::reconcile(const store::OperationRecord& op) {
  (void)op;
  return DispatchResult{DispatchOutcome::Retry, "", "no reconciler registered"};
}

Dispatcher::Dispatcher(store::RunStore& store, std::string run_id, std::string holder)
    : store_(store), run_id_(std::move(run_id)), holder_(std::move(holder)) {}

void Dispatcher::register_handler(std::unique_ptr<OperationHandler> handler) {
  if (handler == nullptr) {
    return;
  }
  const std::string kind(handler->kind());
  handlers_[kind] = std::move(handler);
}

Status Dispatcher::apply_outcome(std::string_view op_id, uint64_t epoch,
                                 const DispatchResult& result, store::UnixTime now) {
  switch (result.outcome) {
    case DispatchOutcome::Confirmed:
      return store_.confirm_operation(op_id, result.result_json, epoch, now);
    case DispatchOutcome::Retry:
      return store_.retry_operation(op_id, now);
    case DispatchOutcome::Failed:
      return store_.fail_operation(op_id, result.detail, now);
    case DispatchOutcome::NeedsReconcile:
      return store_.mark_operation_needs_reconcile(op_id, result.detail, now);
  }
  return fail(error_codes::kNoHandler, "unhandled dispatch outcome");
}

Result<std::size_t> Dispatcher::dispatch_pending(uint64_t epoch, store::UnixTime now) {
  auto pending = store_.list_operations(run_id_, {store::op_state::kPending});
  if (!pending) {
    return tl::make_unexpected(pending.error());
  }
  std::size_t dispatched = 0;
  for (const auto& op : *pending) {
    const auto handler = handlers_.find(op.kind);
    if (handler == handlers_.end()) {
      return fail(error_codes::kNoHandler,
                  "no handler registered for operation kind " + op.kind);
    }
    auto started = store_.mark_operation_started(op.op_id, epoch, now);
    if (!started) {
      return tl::make_unexpected(started.error());
    }
    auto current = store_.get_operation(op.op_id);
    if (!current) {
      return tl::make_unexpected(current.error());
    }
    const DispatchResult result = handler->second->perform(*current);
    auto applied = apply_outcome(op.op_id, epoch, result, now);
    if (!applied) {
      return tl::make_unexpected(applied.error());
    }
    ++dispatched;
  }
  return dispatched;
}

Result<DispatchOutcome> Dispatcher::reconcile_one(std::string_view op_id, uint64_t epoch,
                                                  store::UnixTime now) {
  auto op = store_.get_operation(op_id);
  if (!op) {
    return tl::make_unexpected(op.error());
  }
  const auto handler = handlers_.find(op->kind);
  if (handler == handlers_.end()) {
    // No reconciler: leave it for escalation.
    return DispatchOutcome::NeedsReconcile;
  }
  const DispatchResult result = handler->second->reconcile(*op);
  auto applied = apply_outcome(op_id, epoch, result, now);
  if (!applied) {
    return tl::make_unexpected(applied.error());
  }
  return result.outcome;
}

Result<std::size_t> Dispatcher::reconcile_stale(uint64_t epoch,
                                                std::chrono::seconds grace,
                                                store::UnixTime now) {
  auto started = store_.list_operations(run_id_, {store::op_state::kStarted});
  if (!started) {
    return tl::make_unexpected(started.error());
  }
  std::size_t reconciled = 0;
  for (const auto& op : *started) {
    if (op.epoch >= epoch) {
      continue;  // started under the current epoch: still in flight
    }
    if (op.updated_at + grace.count() > now) {
      continue;  // in-flight grace period has not passed (design 6.12.6 rule 3)
    }
    const auto handler = handlers_.find(op.kind);
    if (handler == handlers_.end()) {
      auto marked = store_.mark_operation_needs_reconcile(
          op.op_id, "no handler registered for operation kind " + op.kind, now);
      if (!marked) {
        return tl::make_unexpected(marked.error());
      }
      ++reconciled;
      continue;
    }
    const DispatchResult result = handler->second->reconcile(op);
    auto applied = apply_outcome(op.op_id, epoch, result, now);
    if (!applied) {
      return tl::make_unexpected(applied.error());
    }
    ++reconciled;
  }
  return reconciled;
}

}  // namespace cf::orch
