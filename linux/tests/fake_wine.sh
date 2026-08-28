#!/usr/bin/env bash
set -euo pipefail
exe=$1; shift
if [ -n "${Q2DZ_TEST_MARKER:-}" ]; then
  printf '%s' "$$" > "${Q2DZ_TEST_MARKER}.wine-pid"
fi
exec "$exe" "$@"
