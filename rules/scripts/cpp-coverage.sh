#!/usr/bin/env bash
# Build and test a CMake, Meson or Make project with Clang coverage
# instrumentation, then export an llvm-cov JSON report for the gate's
# llvm-cov-json parser.
#
# Usage: cpp-coverage.sh <src_dir> <build_dir> <standard_var> <standard_number>
set -euo pipefail

src_dir=$1
build_dir=$2
standard_var=$3
standard_number=$4
covbuild="$build_dir/coverage"
report="$covbuild/coverage.json"
extra_ldflags=${CF_COVERAGE_LDFLAGS:-}

rm -rf "$covbuild"
mkdir -p "$covbuild"

if [[ -f "$src_dir/CMakeLists.txt" ]]; then
  cmake -S "$src_dir" -B "$covbuild" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -D"${standard_var}=${standard_number}" \
    -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate $extra_ldflags" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-instr-generate $extra_ldflags"
  cmake --build "$covbuild" --parallel
  LLVM_PROFILE_FILE="$covbuild/%p.profraw" \
    ctest --test-dir "$covbuild" --output-on-failure
elif [[ -f "$src_dir/meson.build" ]]; then
  setup_args=(
    "$covbuild" "$src_dir"
    --buildtype=debug
    -Dc_args="-fprofile-instr-generate -fcoverage-mapping"
    -Dcpp_args="-fprofile-instr-generate -fcoverage-mapping"
    -Dc_link_args="-fprofile-instr-generate $extra_ldflags"
    -Dcpp_link_args="-fprofile-instr-generate $extra_ldflags"
  )
  if [[ -n "$standard_number" ]]; then
    setup_args+=("-Dc_std=c${standard_number}" "-Dcpp_std=c++${standard_number}")
  fi
  CC=clang CXX=clang++ meson setup "${setup_args[@]}"
  meson compile -C "$covbuild"
  LLVM_PROFILE_FILE="$covbuild/%p.profraw" \
    meson test -C "$covbuild" --print-errorlogs
elif [[ -f "$src_dir/Makefile" || -f "$src_dir/makefile" ]]; then
  covsrc="$covbuild/src"
  mkdir -p "$covsrc"
  tar -C "$src_dir" --exclude=.git --exclude=build --exclude=mutants \
    -cf - . | tar -C "$covsrc" -xf -
  std_cxx=
  std_c=
  if [[ -n "$standard_number" ]]; then
    std_cxx="-std=c++${standard_number}"
    std_c="-std=c${standard_number}"
  fi
  make -C "$covsrc" CC=clang CXX=clang++ \
    CFLAGS="${std_c} -fprofile-instr-generate -fcoverage-mapping" \
    CXXFLAGS="${std_cxx} -fprofile-instr-generate -fcoverage-mapping" \
    LDFLAGS="-fprofile-instr-generate $extra_ldflags"
  LLVM_PROFILE_FILE="$covbuild/%p.profraw" \
    make -C "$covsrc" CC=clang CXX=clang++ test
else
  echo "coverage: no supported build system (CMakeLists.txt, meson.build or Makefile)" >&2
  exit 2
fi

shopt -s nullglob
profiles=("$covbuild"/*.profraw)
if ((${#profiles[@]} == 0)); then
  echo "coverage: no profile data produced by the test run" >&2
  exit 1
fi
llvm-profdata merge -sparse "${profiles[@]}" -o "$covbuild/coverage.profdata"

# llvm-cov ignores objects without coverage mapping, so pass every executable
# in the instrumented build. This avoids depending on CTest's JSON format.
mapfile -d '' objects < <(find "$covbuild" -type f -perm -u+x -print0)
if ((${#objects[@]} == 0)); then
  echo "coverage: no test executables found in $covbuild" >&2
  exit 1
fi
llvm-cov export -instr-profile="$covbuild/coverage.profdata" \
  "${objects[@]}" > "$report"

echo "coverage report: $report"
