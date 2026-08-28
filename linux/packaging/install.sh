#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PAYLOAD="${Q2DZ_PAYLOAD_DIR:-$SCRIPT_DIR/payload}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
INSTALL_ROOT="${Q2DZ_INSTALL_ROOT:-$DATA_HOME/q2dz}"
DESKTOP_HOME="$DATA_HOME/applications"
DESKTOP="$DESKTOP_HOME/q2dz.desktop"
OWNED_MANIFEST="$INSTALL_ROOT/.q2dz-owned-paths"

audit_owned_tree() {
  local actual expected
  actual="$(mktemp -t q2dz-install-actual.XXXXXX)"
  expected="$(mktemp -t q2dz-install-expected.XXXXXX)"
  (cd "$INSTALL_ROOT" && find . -xdev -mindepth 1 -printf '%P\n' | LC_ALL=C sort) > "$actual"
  LC_ALL=C sort -- "$OWNED_MANIFEST" > "$expected"
  if ! cmp -s -- "$actual" "$expected"; then
    unlink "$actual"
    unlink "$expected"
    return 1
  fi
  unlink "$actual"
  unlink "$expected"
}

[[ -f "$PAYLOAD/VERSION" ]] || { echo "invalid package payload" >&2; exit 2; }
for path in bin/q2dz-session bin/q2dz-managed-gateway bin/q2dz-content-check \
  bin/q2dz-webrtc-gateway bin/q2dz-launch \
  lib/libhumblenet.so lib/lib64/libwebrtc.so config/legacy-gateway.json; do
  [[ -f "$PAYLOAD/$path" ]] || { echo "package is incomplete: $path" >&2; exit 2; }
done
if [[ -f "$SCRIPT_DIR/SHA256SUMS" ]]; then
  (cd "$SCRIPT_DIR" && sha256sum -c SHA256SUMS >/dev/null)
fi
command -v ldd >/dev/null 2>&1 || { echo "ldd is required to verify native runtime dependencies" >&2; exit 2; }
for path in bin/q2dz-session bin/q2dz-managed-gateway bin/q2dz-content-check \
  bin/q2dz-webrtc-gateway lib/libhumblenet.so lib/lib64/libwebrtc.so; do
  if ! dependencies="$(ldd "$PAYLOAD/$path" 2>&1)"; then
    echo "cannot inspect native dependencies for $path" >&2
    printf '%s\n' "$dependencies" >&2
    exit 2
  fi
  if grep -Fq 'not found' <<<"$dependencies"; then
    echo "missing native dependencies for $path" >&2
    printf '%s\n' "$dependencies" >&2
    exit 2
  fi
done
case "$INSTALL_ROOT" in ""|/|"$HOME") echo "refusing unsafe install root: $INSTALL_ROOT" >&2; exit 2;; esac
[[ ! -L "$INSTALL_ROOT" ]] || { echo "refusing symlink install root: $INSTALL_ROOT" >&2; exit 2; }
if [[ -e "$INSTALL_ROOT" && ! -f "$INSTALL_ROOT/.q2dz-owned-install" ]]; then
  echo "refusing to replace unowned path: $INSTALL_ROOT" >&2
  exit 2
fi
if [[ -e "$INSTALL_ROOT" ]]; then
  [[ -f "$OWNED_MANIFEST" ]] || {
    echo "existing add-on predates safe updates; uninstall it before installing this package" >&2
    exit 2
  }
  audit_owned_tree || { echo "refusing to update: unknown files exist under $INSTALL_ROOT" >&2; exit 2; }
  # rc8 exposed a second, direct-connect entry point. Remove it only when the
  # installed manifest proves that it belongs to this add-on.
  if grep -Fxq 'bin/q2dz-join' "$OWNED_MANIFEST" && [[ -f "$INSTALL_ROOT/bin/q2dz-join" ]]; then
    if [[ -L "$BIN_HOME/q2dz-join" ]] && \
       [[ "$(readlink -f -- "$BIN_HOME/q2dz-join")" == "$INSTALL_ROOT/bin/q2dz-join" ]]; then
      unlink "$BIN_HOME/q2dz-join"
    fi
    unlink "$INSTALL_ROOT/bin/q2dz-join"
  fi
fi
[[ ! -L "$DESKTOP" ]] || { echo "refusing symlink desktop file: $DESKTOP" >&2; exit 2; }
if [[ -e "$DESKTOP" ]] && ! grep -Fxq 'X-Q2DZ-Owned=true' "$DESKTOP"; then
  echo "refusing to replace unrelated desktop file: $DESKTOP" >&2
  exit 2
fi

mkdir -p "$INSTALL_ROOT" "$BIN_HOME" "$DESKTOP_HOME"
cp -a "$PAYLOAD/." "$INSTALL_ROOT/"
install -m 0755 "$SCRIPT_DIR/uninstall.sh" "$INSTALL_ROOT/uninstall.sh"
install -m 0600 "$PAYLOAD/VERSION" "$INSTALL_ROOT/.q2dz-owned-install"
manifest_tmp="$(mktemp -t q2dz-owned-paths.XXXXXX)"
(
  cd "$INSTALL_ROOT"
  find . -xdev -mindepth 1 ! -path './.q2dz-owned-paths' -printf '%P\n'
  printf '%s\n' '.q2dz-owned-paths'
) | LC_ALL=C sort > "$manifest_tmp"
install -m 0600 "$manifest_tmp" "$OWNED_MANIFEST"
unlink "$manifest_tmp"
for name in q2dz-session q2dz-managed-gateway q2dz-content-check q2dz-webrtc-gateway q2dz-launch; do
  link="$BIN_HOME/$name"
  target="$INSTALL_ROOT/bin/$name"
  if [[ -e "$link" || -L "$link" ]]; then
    if [[ ! -L "$link" ]] || [[ "$(readlink -f -- "$link")" != "$target" ]]; then
      echo "refusing to replace unrelated path: $link" >&2
      exit 2
    fi
  fi
  ln -sfn "$target" "$link"
done
sed "s|^Exec=.*|Exec=$INSTALL_ROOT/bin/q2dz-launch|" \
  "$PAYLOAD/share/applications/q2dz.desktop" > "$DESKTOP.tmp"
install -m 0644 "$DESKTOP.tmp" "$DESKTOP"
unlink "$DESKTOP.tmp"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DESKTOP_HOME" >/dev/null 2>&1 || true
echo "installed Q2PRO-X Dos.Zone add-on: $INSTALL_ROOT"
echo "The installer did not modify Q2PRO-X, Wine configuration, CFG or PAK files."
