#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-sanitizers}"
rm -rf -- "$BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQ2DZ_WERROR=ON \
  -DQ2DZ_ENABLE_SANITIZERS=ON
cmake --build "$BUILD_DIR" --parallel
ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:strict_string_checks=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
ctest --test-dir "$BUILD_DIR" --output-on-failure
echo "sanitizers: PASS"
