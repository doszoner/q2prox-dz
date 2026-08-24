#!/usr/bin/env bash
set -euo pipefail
OUT="${1:?usage: capture-live-snapshot.sh EVIDENCE_DIR SESSION_PID SESSION_DIR}"
SESSION_PID="${2:?usage: capture-live-snapshot.sh EVIDENCE_DIR SESSION_PID SESSION_DIR}"
SESSION_DIR="${3:?usage: capture-live-snapshot.sh EVIDENCE_DIR SESSION_PID SESSION_DIR}"
[[ "$SESSION_PID" =~ ^[1-9][0-9]*$ ]] || {
  echo "SESSION_PID must be a positive integer" >&2
  exit 64
}
session_uid=$(ps -o uid= -p "$SESSION_PID" 2>/dev/null | tr -d ' ')
[[ "$session_uid" == "$(id -u)" ]] || {
  echo "SESSION_PID must identify a current-user process" >&2
  exit 64
}
mkdir -p "$OUT"
date --iso-8601=seconds > "$OUT/time.txt"
uname -a > "$OUT/uname.txt"
{ wine --version 2>/dev/null || true; wine64 --version 2>/dev/null || true; } > "$OUT/wine-version.txt"
{ cmake --version | head -1; c++ --version | head -1; } > "$OUT/toolchain.txt"
if [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
  RUNTIME_BASE="$XDG_RUNTIME_DIR/q2dz"
else
  RUNTIME_BASE="/tmp/q2dz-$(id -u)"
fi
runtime_base=$(realpath -m "$RUNTIME_BASE")
session_dir=$(realpath -e "$SESSION_DIR" 2>/dev/null || true)
case "$session_dir" in
  "$runtime_base"/session-*) ;;
  *)
    echo "SESSION_DIR must identify an existing session under $runtime_base" >&2
    exit 64
    ;;
esac

# Retain only the explicitly selected managed-session process tree. The wrapper
# UI is an ancestor of q2dz-session and is deliberately outside this snapshot.
ps -u "$(id -u)" -eo pid,ppid,pgid,lstart,comm,args | awk -v root="$SESSION_PID" '
  NR == 1 { header = $0; next }
  { line[NR] = $0; pid[NR] = $1; ppid[NR] = $2 }
  END {
    selected[root] = 1
    do {
      changed = 0
      for (i = 2; i <= NR; i++) {
        if (!selected[pid[i]] && selected[ppid[i]]) {
          selected[pid[i]] = 1
          changed = 1
        }
      }
    } while (changed)
    print header
    for (i = 2; i <= NR; i++)
      if (selected[pid[i]]) print line[i]
  }
' | sed -E 's/(--alias )[A-Za-z0-9._-]+/\1[REDACTED]/g' > "$OUT/processes.txt"

pid_list=$(mktemp)
trap 'rm -f "$pid_list"' EXIT
awk 'NR > 1 { print $1 }' "$OUT/processes.txt" > "$pid_list"
ss -u -a -n -p 2>&1 | awk '
  FNR == NR { selected[$1] = 1; next }
  FNR == 1 { print; next }
  {
    for (pid in selected) {
      if ($0 ~ ("pid=" pid "([^0-9]|$)")) {
        print
        next
      }
    }
  }
' "$pid_list" - > "$OUT/udp-sockets.txt" || true
find "$session_dir" -maxdepth 3 -printf '%M %u %g %s %p\n' > "$OUT/runtime-tree.txt"
# Never collect environment, command-line room JSON, browser storage, memory dumps,
# packet payloads, or raw gateway diagnostics that have not passed redaction.
