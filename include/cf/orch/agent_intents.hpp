// cf-orch: applying an agent's MCP intents to the run store (design section
// 6.8.1).
//
// A role's cf-mcp reads a disposable snapshot of the store and never writes
// it. Every write-shaped tool call (file_defect, verify_defect, dispute_test,
// request_waiver) is recorded in the role's calls file with the id the agent
// saw; the orchestrator applies those intents to the real store after the
// turn. This keeps the run store out of the agent's writable mount.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "cf/common/result.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

// Applies the write-shaped calls of one turn in order. `id_map` carries the
// agent-visible defect id to the real store id across the calls of a turn
// (and across continuation attempts); a collision with another milestone's
// id is resolved by allocating a fresh id.
Status apply_agent_intents(store::RunStore& store, std::string_view run_id,
                          std::string_view milestone_id,
                          const std::vector<nlohmann::json>& calls,
                          std::map<std::string, std::string>& id_map,
                          std::uint32_t waiver_deadline_business_days = 2);

}  // namespace cf::orch
