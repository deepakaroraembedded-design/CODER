#!/usr/bin/env bash
# Run mutmut in an isolated copy of the worktree, export CI/CD stats and
# enforce the configured mutation score. The copy keeps the read-only gate
# worktree clean and lets mutmut write its mutant cache under /build.
#
# Usage: python-mutation.sh <src_dir> <build_dir> <min_score>
set -euo pipefail

src_dir=$1
build_dir=$2
min_score=$3
work="$build_dir/mutation"

rm -rf "$work"
mkdir -p "$work"
tar -C "$src_dir" \
  --exclude=.git --exclude=build --exclude=mutants --exclude=.venv \
  -cf - . | tar -C "$work" -xf -
cd "$work"

if ! grep -q '^\[mutmut\]' setup.cfg 2>/dev/null; then
  source_paths=()
  if [[ -d "$work/src" ]]; then
    source_paths=(src)
  elif [[ -d "$work/lib" ]]; then
    source_paths=(lib)
  else
    for entry in "$work"/*.py "$work"/*/; do
      name=$(basename "$entry")
      [[ $name == test* || $name == .* ]] && continue
      [[ -f $entry ]] && source_paths+=("$name")
      [[ -d $entry && -f $entry/__init__.py ]] && source_paths+=("$name")
    done
  fi
  ((${#source_paths[@]} > 0)) || source_paths=(.)
  {
    echo
    echo '[mutmut]'
    echo 'source_paths='
    for path in "${source_paths[@]}"; do echo "    $path"; done
  } >> setup.cfg
fi

mutmut run
mutmut export-cicd-stats
python3 - "$work/mutants/mutmut-cicd-stats.json" "$min_score" <<'PY'
import json
import sys

stats = json.load(open(sys.argv[1], encoding="utf-8"))
total = int(stats.get("total", 0))
killed = int(stats.get("killed", 0))
score = killed / total if total else 1.0
minimum = float(sys.argv[2])
print(f"mutation score: {score:.2%} ({killed}/{total}), minimum {minimum:.2%}")
sys.exit(0 if score >= minimum else 1)
PY
