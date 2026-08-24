#!/usr/bin/env bash
set -euo pipefail
exe=$1; shift
exec "$exe" "$@"
