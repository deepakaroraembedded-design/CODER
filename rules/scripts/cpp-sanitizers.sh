#!/usr/bin/env bash
# Build and test a CMake, Meson or Make project with ASan+UBSan
# instrumentation (design section 6.7 stage 3).
#
# Usage: cpp-sanitizers.sh <src_dir> <build_dir> <standard_var> <standard_number>
set -euo pipefail

src_dir=$1
build_dir=$2
standard_var=$3
standard_number=$4
sanbuild="$build_dir/sanitizers"
san_flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

rm -rf "$sanbuild"
mkdir -p "$sanbuild"

if [[ -f "$src_dir/CMakeLists.txt" ]]; then
  cmake -S "$src_dir" -B "$sanbuild" \
    -DCMAKE_BUILD_TYPE=Debug \
    -D"${standard_var}=${standard_number}" \
    -DCMAKE_C_FLAGS="$san_flags" \
    -DCMAKE_CXX_FLAGS="$san_flags" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build "$sanbuild" --parallel
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    ctest --test-dir "$sanbuild" --output-on-failure
elif [[ -f "$src_dir/meson.build" ]]; then
  setup_args=(
    "$sanbuild" "$src_dir"
    --buildtype=debug
    -Dc_args="$san_flags"
    -Dcpp_args="$san_flags"
    -Dc_link_args="-fsanitize=address,undefined"
    -Dcpp_link_args="-fsanitize=address,undefined"
  )
  if [[ -n "$standard_number" ]]; then
    setup_args+=("-Dc_std=c${standard_number}" "-Dcpp_std=c++${standard_number}")
  fi
  meson setup "${setup_args[@]}"
  meson compile -C "$sanbuild"
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    meson test -C "$sanbuild" --print-errorlogs
elif [[ -f "$src_dir/Makefile" || -f "$src_dir/makefile" ]]; then
  sansrc="$sanbuild/src"
  mkdir -p "$sansrc"
  tar -C "$src_dir" --exclude=.git --exclude=build --exclude=mutants \
    -cf - . | tar -C "$sansrc" -xf -
  std_cxx=
  std_c=
  if [[ -n "$standard_number" ]]; then
    std_cxx="-std=c++${standard_number}"
    std_c="-std=c${standard_number}"
  fi
  make -C "$sansrc" \
    CFLAGS="${std_c} ${san_flags}" \
    CXXFLAGS="${std_cxx} ${san_flags}" \
    LDFLAGS="-fsanitize=address,undefined"
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    make -C "$sansrc" test
else
  echo "sanitizers: no supported build system (CMakeLists.txt, meson.build or Makefile)" >&2
  exit 2
fi
