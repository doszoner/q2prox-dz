#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
PROJECT_ROOT="$(cd -- "$ROOT/.." && pwd -P)"
VERSION="$(<"$PROJECT_ROOT/VERSION")"
NAME="q2prox-dz-$VERSION"
OUT="${1:-$ROOT/dist}"
if [[ -z "${Q2DZ_WEBRTC_NET_RELEASE_DIR:-}" ]]; then
  echo "Q2DZ_WEBRTC_NET_RELEASE_DIR is required (path to the pinned WebRTC-NET release)" >&2
  exit 2
fi
WEBRTC_RELEASE="$(realpath -e -- "$Q2DZ_WEBRTC_NET_RELEASE_DIR")" || {
  echo "Q2DZ_WEBRTC_NET_RELEASE_DIR does not exist: $Q2DZ_WEBRTC_NET_RELEASE_DIR" >&2
  exit 2
}
WEBRTC_SOURCE_INPUT="${Q2DZ_WEBRTC_NET_SOURCE_DIR:-$(cd -- "$ROOT/../.." && pwd -P)/WebRTC-NET}"
WEBRTC_SOURCE="$(realpath -e -- "$WEBRTC_SOURCE_INPUT")" || {
  echo "Q2DZ_WEBRTC_NET_SOURCE_DIR does not exist: $WEBRTC_SOURCE_INPUT" >&2
  exit 2
}
[[ -d "$WEBRTC_RELEASE" && -d "$WEBRTC_SOURCE" ]] || {
  echo "WebRTC-NET release and source inputs must be directories" >&2
  exit 2
}
python3 "$PROJECT_ROOT/tools/verify-dependencies.py" \
  --lock "$PROJECT_ROOT/deps.lock.json" \
  --linux-release "$WEBRTC_RELEASE" \
  --webrtc-source "$WEBRTC_SOURCE"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1787356800}"
WORK="$(mktemp -d -t q2dz-package.XXXXXX)"
PACKAGE="$WORK/$NAME"
ARCHIVE="$OUT/$NAME-linux-x86_64.tar.gz"

Q2DZ_WEBRTC_NET_RELEASE_DIR="$WEBRTC_RELEASE" "$ROOT/tools/build-release.sh"
mkdir -p "$PACKAGE/payload" "$OUT"
cmake --install "$ROOT/build-release" --prefix "$PACKAGE/payload" --strip
install -m 0755 "$ROOT/packaging/install.sh" "$PACKAGE/install.sh"
install -m 0755 "$ROOT/packaging/uninstall.sh" "$PACKAGE/uninstall.sh"
install -m 0644 "$ROOT/README.md" "$PACKAGE/README.md"
install -m 0644 "$ROOT/THIRD-PARTY-NOTICES.md" "$PACKAGE/THIRD-PARTY-NOTICES.md"
mkdir -p "$PACKAGE/LICENSES"
install -m 0644 "$ROOT/../LICENSE" "$PACKAGE/LICENSES/q2prox-dz-LICENSE.txt"
install -m 0644 "$ROOT/../LICENSE-DOCS" "$PACKAGE/LICENSES/q2prox-dz-LICENSE-DOCS.txt"
install -m 0644 "$ROOT/../LICENSE-CONFIGS" "$PACKAGE/LICENSES/q2prox-dz-LICENSE-CONFIGS.txt"
install -m 0644 "$WEBRTC_SOURCE/LICENSE.md" "$PACKAGE/LICENSES/WebRTC-NET-LICENSE.md"
install -m 0644 "$WEBRTC_SOURCE/NOTICES.md" "$PACKAGE/LICENSES/WebRTC-NET-NOTICES.md"
install -m 0644 "$WEBRTC_SOURCE/3rdparty/webrtc-native-build/LICENSE" \
  "$PACKAGE/LICENSES/webrtc-native-build-LICENSE.txt"
install -m 0644 "$WEBRTC_SOURCE/3rdparty/webrtc/license.txt" \
  "$PACKAGE/LICENSES/Chromium-WebRTC-LICENSE.txt"
(cd "$PACKAGE" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)

TZ=UTC LC_ALL=C tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" \
  --owner=0 --group=0 --numeric-owner -C "$WORK" -cf - "$NAME" | gzip -n > "$ARCHIVE.tmp"
mv "$ARCHIVE.tmp" "$ARCHIVE"
(cd "$OUT" && sha256sum "$(basename -- "$ARCHIVE")" > "$(basename -- "$ARCHIVE").sha256")
echo "$ARCHIVE"
