#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
SRC="$ROOT/src"
CFG="$ROOT/config/legacy-gateway.json"
TRANSPORT="$ROOT/transport/q2_doszone_gateway.cpp"

fail() { echo "security invariant failed: $*" >&2; exit 1; }

if grep -RInE '\b(system|popen|execlp?)\s*\(' "$SRC"; then
  fail "shell/process helper forbidden; use direct execve/posix_spawn semantics"
fi
if grep -RInE 'INADDR_ANY|0\.0\.0\.0|AF_INET6' "$SRC"; then
  fail "gateway must remain IPv4 loopback-only for this product"
fi
if grep -RInE 'mkstemp|tmpnam|tmpfile|ice.*ofstream|room.*ofstream' "$SRC"; then
  fail "room or ICE data must not be persisted"
fi
for required in 'memfd_create' 'F_ADD_SEALS' 'PR_SET_DUMPABLE' 'RLIMIT_CORE' '127.0.0.1' 'TRANSPORT_READY' 'Q2DZ_SESSION_ID'; do
  grep -Rqs "$required" "$SRC" || fail "missing required source invariant: $required"
done
python3 - "$CFG" <<'PY'
import json,sys
cfg=json.load(open(sys.argv[1],encoding='utf-8'))
argv=cfg.get('argv',[])
joined='\n'.join(argv)
required=['{alias}','{ice_memfd}','{internal_endpoint}']
for item in required:
    if item not in joined: raise SystemExit(f'missing {item}')
for item in ('{ice_json}','{credential}','{token}','{secret}'):
    if item in joined: raise SystemExit(f'forbidden placeholder {item}')
expected={'--bind','--signal','--ice-file','--alias','--max-datagram'}
actual={item for item in argv if item.startswith('--')}
if actual != expected: raise SystemExit(f'unexpected transport options: {sorted(actual)}')
if 'wss://net.js-dos.com:444' not in argv: raise SystemExit('missing pinned signaling endpoint')
PY
grep -q 'message ? message : "unknown HumbleNet error"' "$TRANSPORT" ||
  fail "HumbleNet nullable error fallback is missing"
! grep -q 'runtime_error(humblenet_get_error())' "$TRANSPORT" ||
  fail "nullable humblenet_get_error passed directly to runtime_error"

echo "security invariants: PASS"
