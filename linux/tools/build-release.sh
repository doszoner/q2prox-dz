#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-release}"
WEBRTC_RELEASE="${Q2DZ_WEBRTC_NET_RELEASE_DIR:?set Q2DZ_WEBRTC_NET_RELEASE_DIR to the pinned WebRTC-NET release directory}"

rm -rf -- "$BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DQ2DZ_WERROR=ON \
  -DQ2DZ_REPRODUCIBLE=ON \
  -DQ2DZ_WEBRTC_NET_RELEASE_DIR="$WEBRTC_RELEASE" \
  -DQ2DZ_REQUIRE_WEBRTC_GATEWAY="${Q2DZ_REQUIRE_WEBRTC_GATEWAY:-ON}"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
"$ROOT/tools/check-security-invariants.sh"

python3 -m py_compile "$ROOT"/tools/*.py "$ROOT"/tools/q2dz-launch "$ROOT"/tests/*.py
for script in "$ROOT"/tools/*.sh "$ROOT"/tests/*.sh; do
  bash -n "$script"
done

check_elf() {
  local binary="$1"
  file "$binary"
  readelf -W -h "$binary" | grep -Eq 'Type:[[:space:]]+DYN' || {
    echo "FAIL non-PIE executable: $binary" >&2; return 1;
  }
  readelf -W -l "$binary" | grep -q 'GNU_RELRO' || {
    echo "FAIL missing GNU_RELRO: $binary" >&2; return 1;
  }
  readelf -W -l "$binary" | awk '/GNU_STACK/ { if ($0 ~ /E/) exit 1; found=1 } END { exit found ? 0 : 1 }' || {
    echo "FAIL executable or missing GNU_STACK: $binary" >&2; return 1;
  }
  readelf -W -d "$binary" | grep -Eq 'BIND_NOW|FLAGS.*NOW' || {
    echo "FAIL missing immediate binding: $binary" >&2; return 1;
  }
}

for binary in q2dz-session q2dz-managed-gateway q2dz-content-check q2dz-webrtc-gateway; do
  check_elf "$BUILD_DIR/$binary"
done
"$ROOT/tools/verify-legacy-contract.py" \
  --gateway "$BUILD_DIR/q2dz-webrtc-gateway" \
  --adapter "$ROOT/config/legacy-gateway.json"
sha256sum "$BUILD_DIR/q2dz-session" "$BUILD_DIR/q2dz-managed-gateway" \
  "$BUILD_DIR/q2dz-content-check" "$BUILD_DIR/q2dz-webrtc-gateway"
echo "release build: PASS"
