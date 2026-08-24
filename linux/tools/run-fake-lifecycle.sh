#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-fake-lifecycle}"
RUNS="${Q2DZ_LIFECYCLE_RUNS:-20}"
LOG_DIR="${Q2DZ_LIFECYCLE_LOG_DIR:-$(mktemp -d -t q2dz-fake-lifecycle.XXXXXX)}"
[[ "$RUNS" =~ ^[1-9][0-9]*$ ]] || { echo "Q2DZ_LIFECYCLE_RUNS must be a positive integer" >&2; exit 2; }
mkdir -p -- "$LOG_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQ2DZ_WERROR=ON
cmake --build "$BUILD_DIR" --parallel
for run in $(seq 1 "$RUNS"); do
  log="$LOG_DIR/run-$(printf '%02d' "$run").log"
  if ctest --test-dir "$BUILD_DIR" -R 'unit|fake-lifecycle' --output-on-failure >"$log" 2>&1; then
    echo "fake lifecycle: PASS run=$run/$RUNS log=$log"
  else
    rc=$?
    cat "$log" >&2
    echo "fake lifecycle: FAIL run=$run/$RUNS log=$log" >&2
    exit "$rc"
  fi
done
echo "fake lifecycle repeats: PASS runs=$RUNS logs=$LOG_DIR"
