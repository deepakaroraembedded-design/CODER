#!/usr/bin/env bash
# Run Stryker mutation testing for a TypeScript/JavaScript project and enforce
# the configured mutation score. Stryker is invoked from the target project so
# it can find its installed test runner and plugins.
#
# Usage: ts-mutation.sh <src_dir> <build_dir> <min_ratio>
set -euo pipefail

src_dir=$1
build_dir=$2
min_ratio=$3
report="$build_dir/stryker-mutation.json"
config="$build_dir/stryker.config.json"

if [[ ! -f "$src_dir/package.json" ]]; then
  echo "mutation: no package.json in $src_dir" >&2
  exit 2
fi
test_runner=vitest
if ! grep -q '"vitest"' "$src_dir/package.json" 2>/dev/null; then
  if grep -q '"jest"' "$src_dir/package.json" 2>/dev/null; then
    test_runner=jest
  fi
fi
package_manager=npm
if [[ -f "$src_dir/pnpm-lock.yaml" ]]; then
  package_manager=pnpm
elif [[ -f "$src_dir/yarn.lock" ]]; then
  package_manager=yarn
fi
mkdir -p "$build_dir"
cat > "$config" <<JSON
{
  "packageManager": "$package_manager",
  "testRunner": "$test_runner",
  "mutate": ["src/**/*.ts", "src/**/*.tsx", "src/**/*.js", "src/**/*.jsx"],
  "reporters": ["json"],
  "jsonReporter": { "fileName": "$report" }
}
JSON

(cd "$src_dir" && npx --yes stryker run "$config")
if [[ ! -f "$report" ]]; then
  echo "mutation: Stryker report not found at $report" >&2
  exit 1
fi
node -e '
const fs = require("fs");
const report = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
let killed = 0, survived = 0, timeout = 0;
for (const file of Object.values(report.files || {})) {
  for (const mutant of file.mutants || []) {
    if (mutant.status === "Killed") killed++;
    else if (mutant.status === "Survived") survived++;
    else if (mutant.status === "Timeout") timeout++;
  }
}
const total = killed + survived + timeout;
const score = total === 0 ? 100 : (100 * killed / total);
const minimum = Number(process.argv[2]) * 100;
console.log(`mutation score: ${score.toFixed(2)}%, minimum ${minimum.toFixed(2)}%`);
process.exit(score + 1e-9 >= minimum ? 0 : 1);
' "$report" "$min_ratio"
