#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
PROJECT_ROOT="$(cd -- "$ROOT/.." && pwd -P)"
if [[ -z "${Q2DZ_WEBRTC_NET_RELEASE_DIR:-}" ]]; then
  echo "Q2DZ_WEBRTC_NET_RELEASE_DIR is required (path to the pinned WebRTC-NET release)" >&2
  exit 2
fi
WEBRTC_RELEASE="$(realpath -e -- "$Q2DZ_WEBRTC_NET_RELEASE_DIR")" || {
  echo "Q2DZ_WEBRTC_NET_RELEASE_DIR does not name an existing directory: $Q2DZ_WEBRTC_NET_RELEASE_DIR" >&2
  exit 2
}
[[ -d "$WEBRTC_RELEASE" ]] || {
  echo "Q2DZ_WEBRTC_NET_RELEASE_DIR is not a directory: $WEBRTC_RELEASE" >&2
  exit 2
}
python3 "$PROJECT_ROOT/tools/verify-dependencies.py" \
  --lock "$PROJECT_ROOT/deps.lock.json" \
  --linux-release "$WEBRTC_RELEASE"

BUILD_PARENT=""
if [[ -n "${BUILD_DIR:-}" ]]; then
  mkdir -p -- "$BUILD_DIR"
  BUILD_DIR="$(realpath -e -- "$BUILD_DIR")"
  if [[ -n "$(find "$BUILD_DIR" -mindepth 1 -print -quit)" ]]; then
    echo "BUILD_DIR must be empty for a clean source install: $BUILD_DIR" >&2
    exit 2
  fi
else
  BUILD_PARENT="$(mktemp -d -t q2dz-source-build.XXXXXX)"
  BUILD_DIR="$BUILD_PARENT/build"
fi
STAGE="$(mktemp -d -t q2dz-source-install.XXXXXX)"
cleanup() {
  rm -rf -- "$STAGE"
  [[ -z "$BUILD_PARENT" ]] || rm -rf -- "$BUILD_PARENT"
}
trap cleanup EXIT

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQ2DZ_WERROR=ON -DQ2DZ_REPRODUCIBLE=ON -DQ2DZ_REQUIRE_WEBRTC_GATEWAY=ON \
  -DQ2DZ_WEBRTC_NET_RELEASE_DIR="$WEBRTC_RELEASE"
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
cmake --install "$BUILD_DIR" --prefix "$STAGE/payload" --strip
Q2DZ_PAYLOAD_DIR="$STAGE/payload" "$ROOT/packaging/install.sh"
