// cf-gates: process execution for trusted gate steps (design section 6.7.3).
//
// The executor lives in cf-common (cf/common/process.hpp) so git and other
// modules can reuse it; these aliases keep the gates-facing names.
#pragma once

#include "cf/common/process.hpp"

namespace cf::gates {

using ProcessRequest = cf::process::ProcessRequest;
using ProcessResult = cf::process::ProcessResult;
using ProcessExecutor = cf::process::ProcessExecutor;
using LocalProcessExecutor = cf::process::LocalProcessExecutor;

}  // namespace cf::gates
