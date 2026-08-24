#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION="$(<"$ROOT/../VERSION")"
NAME="q2prox-dz-$VERSION"
TMP="$(mktemp -d -t q2dz-package-smoke.XXXXXX)"
OUT="$TMP/out"
HOME_DIR="$TMP/home"
mkdir -p "$OUT" "$HOME_DIR"

fake_bin="$TMP/fake-bin"
mkdir -p "$fake_bin"
printf '#!/bin/sh\n: > "$CMAKE_MARKER"\nexit 99\n' > "$fake_bin/cmake"
chmod 0755 "$fake_bin/cmake"

stale_build="$TMP/stale-build"
mkdir -p "$stale_build"
touch "$stale_build/CMakeCache.txt"
ln -s "$stale_build" "$TMP/build-link"
if CMAKE_MARKER="$TMP/install-cmake-ran" PATH="$fake_bin:$PATH" \
  BUILD_DIR="$TMP/build-link" \
  "$ROOT/tools/install-user.sh" >"$TMP/install-stale.log" 2>&1; then
  echo "install-user accepted a stale symlinked BUILD_DIR" >&2
  exit 1
fi
[[ ! -e "$TMP/install-cmake-ran" ]]
grep -Fq 'BUILD_DIR must be empty for a clean source install' "$TMP/install-stale.log"

empty_build="$TMP/empty-build"
mkdir -p "$empty_build"
if CMAKE_MARKER="$TMP/install-empty-cmake-ran" PATH="$fake_bin:$PATH" \
  BUILD_DIR="$empty_build" \
  "$ROOT/tools/install-user.sh" >"$TMP/install-empty.log" 2>&1; then
  echo "fake cmake unexpectedly succeeded" >&2
  exit 1
fi
[[ -e "$TMP/install-empty-cmake-ran" && -d "$empty_build" ]]

valid_release="$(realpath -e -- "$Q2DZ_WEBRTC_NET_RELEASE_DIR")"
valid_source="$(realpath -e -- "${Q2DZ_WEBRTC_NET_SOURCE_DIR:-$(cd -- "$ROOT/../.." && pwd -P)/WebRTC-NET}")"
for case in runtime source; do
  release="$valid_release"
  source="$valid_source"
  [[ "$case" != runtime ]] || release="$ROOT"
  [[ "$case" != source ]] || source="$ROOT/.."
  if CMAKE_MARKER="$TMP/package-$case-cmake-ran" PATH="$fake_bin:$PATH" \
    BUILD_DIR="$TMP/package-$case-build" \
    Q2DZ_WEBRTC_NET_RELEASE_DIR="$release" \
    Q2DZ_WEBRTC_NET_SOURCE_DIR="$source" \
    "$ROOT/tools/build-package.sh" "$TMP/package-$case-out" \
      >"$TMP/package-$case.log" 2>&1; then
    echo "build-package accepted mismatched WebRTC-NET $case" >&2
    exit 1
  fi
  [[ ! -e "$TMP/package-$case-cmake-ran" ]]
  grep -Fq 'dependency verification failed:' "$TMP/package-$case.log"
done

"$ROOT/tools/build-package.sh" "$OUT"
archive="$OUT/$NAME-linux-x86_64.tar.gz"
[[ -f "$archive" && -f "$archive.sha256" ]]
(cd "$OUT" && sha256sum -c "$(basename -- "$archive.sha256")")
mkdir -p "$TMP/unpacked"
tar -C "$TMP/unpacked" -xzf "$archive"
package="$TMP/unpacked/$NAME"
for license in q2prox-dz-LICENSE.txt q2prox-dz-LICENSE-DOCS.txt \
  q2prox-dz-LICENSE-CONFIGS.txt WebRTC-NET-LICENSE.md WebRTC-NET-NOTICES.md \
  webrtc-native-build-LICENSE.txt Chromium-WebRTC-LICENSE.txt; do
  [[ -s "$package/LICENSES/$license" ]]
done

run_env=(env HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_RUNTIME_DIR="$TMP/runtime")
fake_bin="$TMP/fake-bin"
mkdir -p "$fake_bin"
printf '#!/bin/sh\nprintf "libmissing.so => not found\\n"\n' > "$fake_bin/ldd"
chmod 0755 "$fake_bin/ldd"
if env PATH="$fake_bin:$PATH" "${run_env[@]}" "$package/install.sh"; then
  echo "installer ignored a missing native dependency" >&2
  exit 1
fi
[[ ! -e "$HOME_DIR/.local/share/q2dz" ]]
mkdir -p "$HOME_DIR/.local/share/applications"
printf 'user-owned desktop\n' > "$HOME_DIR/.local/share/applications/q2dz.desktop"
if "${run_env[@]}" "$package/install.sh"; then
  echo "installer replaced an unrelated desktop file" >&2
  exit 1
fi
grep -Fq 'user-owned desktop' "$HOME_DIR/.local/share/applications/q2dz.desktop"
unlink "$HOME_DIR/.local/share/applications/q2dz.desktop"
"${run_env[@]}" "$package/install.sh"
"${run_env[@]}" "$package/install.sh"

install_root="$HOME_DIR/.local/share/q2dz"
for name in q2dz-session q2dz-managed-gateway q2dz-content-check q2dz-webrtc-gateway q2dz-launch q2dz-join; do
  [[ -x "$install_root/bin/$name" ]]
  [[ -L "$HOME_DIR/.local/bin/$name" ]]
done
[[ -f "$install_root/lib/libhumblenet.so" ]]
[[ -f "$install_root/lib/lib64/libwebrtc.so" ]]
[[ -x "$install_root/uninstall.sh" ]]
desktop="$HOME_DIR/.local/share/applications/q2dz.desktop"
grep -Fq "Exec=$install_root/bin/q2dz-launch" "$desktop"
grep -Fxq 'X-Q2DZ-Owned=true' "$desktop"
"$install_root/bin/q2dz-session" --help >/dev/null
"$install_root/bin/q2dz-webrtc-gateway" --help >/dev/null
"$install_root/bin/q2dz-launch" --help >/dev/null

printf 'user-owned\n' > "$install_root/user-note.txt"
if "${run_env[@]}" "$install_root/uninstall.sh"; then
  echo "uninstaller removed an unknown user file" >&2
  exit 1
fi
[[ -f "$install_root/user-note.txt" ]]
unlink "$install_root/user-note.txt"
"${run_env[@]}" "$install_root/uninstall.sh"
[[ ! -e "$install_root" ]]
[[ ! -e "$desktop" ]]
[[ ! -e "$HOME_DIR/.local/bin/q2dz-launch" ]]
echo "package install/update/uninstall smoke: PASS"
