// cf-orch: apply human waivers to gate results (design section 10.2).
//
// A waiver names a gate rule and a scope ("src/bus.cpp:42" or "*"). It
// covers the findings of that gate whose message names the scope, and a gate
// is waived only when every one of its findings is covered. Skipped gates are
// never waived: a waiver cannot hide checks that never ran.
#pragma once

#include <vector>

#include "cf/gates/runner.hpp"
#include "cf/store/run_store.hpp"

namespace cf::orch {

// Marks the covered failed gates as waived and recomputes report.passed.
// Gates that are already passed are left alone.
void apply_waivers(gates::GateReport& report,
                   const std::vector<store::WaiverRecord>& waivers);

}  // namespace cf::orch
