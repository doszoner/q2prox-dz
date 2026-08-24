#!/usr/bin/env bash
set -euo pipefail
if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
  DEFAULT_RUNTIME_BASE="$XDG_RUNTIME_DIR/q2dz"
else
  DEFAULT_RUNTIME_BASE="/tmp/q2dz-$(id -u)"
fi
RUNTIME_BASE="${1:-$DEFAULT_RUNTIME_BASE}"
failed=0

# Compare argv[0], not a pgrep substring that can match this check's parent shell.
for proc in /proc/[0-9]*; do
  [[ "$(stat -c %u "$proc" 2>/dev/null || true)" == "$(id -u)" ]] || continue
  argv0=""
  IFS= read -r -d '' argv0 < "$proc/cmdline" 2>/dev/null || true
  case "${argv0##*/}" in
    q2dz-session|q2dz-managed-gateway|q2dz-webrtc-gateway|Q2PRO-X.exe)
      echo "FAIL orphan process: ${proc##*/} ${argv0##*/}" >&2
      failed=1
      ;;
  esac
done

if command -v ss >/dev/null 2>&1; then
  sockets="$(ss -H -u -l -p 2>/dev/null | grep -E 'q2dz|Q2PRO|wine' || true)"
  if [[ -n "$sockets" ]]; then
    echo "FAIL orphan UDP socket" >&2
    echo "$sockets" >&2
    failed=1
  fi
fi

if [[ -d "$RUNTIME_BASE" ]]; then
  while IFS= read -r -d '' d; do
    [[ -f "$d/.q2dz-session" ]] || continue
    echo "FAIL stale marked session directory: $d" >&2
    failed=1
  done < <(find "$RUNTIME_BASE" -mindepth 1 -maxdepth 1 -type d -print0)
fi

if (( failed )); then exit 1; fi
echo "orphan check: PASS"
