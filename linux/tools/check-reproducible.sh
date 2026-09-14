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
python3 "$PROJECT_ROOT/tools/verify-dependencies.py" \
  --lock "$PROJECT_ROOT/deps.lock.json" \
  --linux-release "$WEBRTC_RELEASE"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1787356800}"
TMP="$(mktemp -d -t q2dz-repro.XXXXXX)"
trap 'rm -rf -- "$TMP"' EXIT

copy_source() {
  local dst="$1"
  mkdir -p "$dst"
  tar -C "$ROOT" \
    --exclude='./build*' --exclude='./dist' --exclude='./.git' \
    -cf - . | tar -C "$dst" -xf -
  install -m 0644 "$PROJECT_ROOT/VERSION" "$dst/../VERSION"
  touch -h -d "@$SOURCE_DATE_EPOCH" "$dst/../VERSION"
  find "$dst" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
}

build_one() {
  local src="$1" build="$2" stage="$3"
  SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" TZ=UTC LC_ALL=C \
  cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DQ2DZ_WERROR=ON \
    -DQ2DZ_REPRODUCIBLE=ON \
    -DQ2DZ_REQUIRE_WEBRTC_GATEWAY=ON \
    -DQ2DZ_WEBRTC_NET_RELEASE_DIR="$WEBRTC_RELEASE" >/dev/null
  SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" TZ=UTC LC_ALL=C \
  cmake --build "$build" --parallel >/dev/null
  DESTDIR="$stage" cmake --install "$build" --strip >/dev/null
}

copy_source "$TMP/a/src"
copy_source "$TMP/b/src"
build_one "$TMP/a/src" "$TMP/a/build" "$TMP/a/stage"
build_one "$TMP/b/src" "$TMP/b/build" "$TMP/b/stage"

for binary in q2dz-session q2dz-managed-gateway q2dz-content-check q2dz-webrtc-gateway; do
  cmp "$TMP/a/stage/usr/bin/$binary" "$TMP/b/stage/usr/bin/$binary"
done

( cd "$TMP/a/stage" && find . -type f -print0 | sort -z | xargs -0 sha256sum ) > "$TMP/a.sha"
( cd "$TMP/b/stage" && find . -type f -print0 | sort -z | xargs -0 sha256sum ) > "$TMP/b.sha"
cmp "$TMP/a.sha" "$TMP/b.sha"
cat "$TMP/a.sha"
echo "reproducible build: PASS"
