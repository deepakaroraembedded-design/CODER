#!/usr/bin/env bash
# Run Mull mutation testing on a CMake project and enforce the configured
# mutation score. The toolchain image provides mull-runner and the LLVM plugin;
# override MULL_RUNNER / MULL_FRONTEND for a different installation.
#
# Usage: cpp-mutation.sh <src_dir> <build_dir> <min_ratio>
set -euo pipefail

src_dir=$1
build_dir=$2
min_ratio=$3
covbuild="$build_dir/mutation"

if [[ ! -f "$src_dir/CMakeLists.txt" && ! -f "$src_dir/meson.build" &&
      ! -f "$src_dir/Makefile" && ! -f "$src_dir/makefile" ]]; then
  echo "mutation: no supported C/C++ build (CMake, Meson or Make)" >&2
  exit 2
fi

runner=${MULL_RUNNER:-}
if [[ -z "$runner" ]]; then
  for candidate in mull-runner-18 mull-runner-17 mull-runner; do
    if command -v "$candidate" >/dev/null 2>&1; then runner=$candidate; break; fi
  done
fi
frontend=${MULL_FRONTEND:-}
if [[ -z "$frontend" ]]; then
  for candidate in /usr/lib/mull-ir-frontend-18 /usr/lib/mull-ir-frontend-17 \
                   /usr/lib/mull-ir-frontend; do
    if [[ -f "$candidate" ]]; then frontend=$candidate; break; fi
  done
fi
if [[ -z "$runner" || -z "$frontend" ]]; then
  echo "mutation: Mull runner/frontend not found (set MULL_RUNNER/MULL_FRONTEND)" >&2
  exit 2
fi

cc=${CC:-clang}
cxx=${CXX:-clang++}
rm -rf "$covbuild"
mkdir -p "$covbuild"
if [[ -f "$src_dir/CMakeLists.txt" ]]; then
  cmake -S "$src_dir" -B "$covbuild" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_C_FLAGS="-O0 -g -grecord-command-line -fpass-plugin=$frontend" \
    -DCMAKE_CXX_FLAGS="-O0 -g -grecord-command-line -fpass-plugin=$frontend"
  cmake --build "$covbuild" --parallel
elif [[ -f "$src_dir/meson.build" ]]; then
  CC="$cc" CXX="$cxx" meson setup "$covbuild" "$src_dir" --buildtype=debug \
    -Dc_args="-O0 -g -grecord-command-line -fpass-plugin=$frontend" \
    -Dcpp_args="-O0 -g -grecord-command-line -fpass-plugin=$frontend"
  meson compile -C "$covbuild"
else
  covsrc="$covbuild/src"
  mkdir -p "$covsrc"
  tar -C "$src_dir" --exclude=.git --exclude=build --exclude=mutants \
    -cf - . | tar -C "$covsrc" -xf -
  make -C "$covsrc" CC="$cc" CXX="$cxx" \
    CFLAGS="-O0 -g -grecord-command-line -fpass-plugin=$frontend" \
    CXXFLAGS="-O0 -g -grecord-command-line -fpass-plugin=$frontend"
fi

required=$(awk -v value="$min_ratio" 'BEGIN { printf "%.2f", value * 100 }')
failed=0
observed=0
mapfile -d '' exes < <(find "$covbuild" -type f -perm -u+x -print0)
for exe in "${exes[@]}"; do
  output=$("$runner" "$exe" 2>&1 || true)
  score=$(grep -oE 'Mutation score: [0-9]+' <<<"$output" | tail -1 | awk '{print $3}')
  [[ -n "$score" ]] || continue
  observed=$((observed + 1))
  printf '%s: mutation score %s%%\n' "$exe" "$score"
  if ! awk -v score="$score" -v minimum="$required"       'BEGIN { exit !(score + 0 >= minimum + 0) }'; then
    failed=1
  fi
done
if ((observed == 0)); then
  echo "mutation: Mull found no mutable test targets (treated as 100%)"
  exit 0
fi
if ((failed)); then
  echo "mutation: score below minimum ${required}%" >&2
  exit 1
fi
echo "mutation: all targets met minimum ${required}%"
