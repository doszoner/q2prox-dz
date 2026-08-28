#!/usr/bin/env bash
set -euo pipefail
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
INSTALL_ROOT="${Q2DZ_INSTALL_ROOT:-$DATA_HOME/q2dz}"
DESKTOP="$DATA_HOME/applications/q2dz.desktop"
MARKER="$INSTALL_ROOT/.q2dz-owned-install"
OWNED_MANIFEST="$INSTALL_ROOT/.q2dz-owned-paths"

[[ -f "$MARKER" ]] || { echo "refusing to remove unowned path: $INSTALL_ROOT" >&2; exit 2; }
[[ -f "$OWNED_MANIFEST" ]] || { echo "refusing to remove install without owned-path manifest" >&2; exit 2; }
case "$INSTALL_ROOT" in ""|/|"$HOME") echo "refusing unsafe install root: $INSTALL_ROOT" >&2; exit 2;; esac
[[ ! -L "$INSTALL_ROOT" ]] || { echo "refusing symlink install root: $INSTALL_ROOT" >&2; exit 2; }
actual="$(mktemp -t q2dz-uninstall-actual.XXXXXX)"
expected="$(mktemp -t q2dz-uninstall-expected.XXXXXX)"
(cd "$INSTALL_ROOT" && find . -xdev -mindepth 1 -printf '%P\n' | LC_ALL=C sort) > "$actual"
LC_ALL=C sort -- "$OWNED_MANIFEST" > "$expected"
if ! cmp -s -- "$actual" "$expected"; then
  unlink "$actual"
  unlink "$expected"
  echo "refusing to uninstall: unknown files exist under $INSTALL_ROOT" >&2
  exit 2
fi
unlink "$actual"
unlink "$expected"
for name in q2dz-session q2dz-managed-gateway q2dz-content-check q2dz-webrtc-gateway q2dz-launch; do
  link="$BIN_HOME/$name"
  if [[ -L "$link" ]] && [[ "$(readlink -f -- "$link")" == "$INSTALL_ROOT/bin/$name" ]]; then
    unlink "$link"
  fi
done
if [[ -f "$DESKTOP" ]] && grep -Fxq 'X-Q2DZ-Owned=true' "$DESKTOP" && \
   grep -Fxq "Exec=$INSTALL_ROOT/bin/q2dz-launch" "$DESKTOP"; then
  unlink "$DESKTOP"
fi
find "$INSTALL_ROOT" -xdev -depth -delete
echo "removed add-on-owned files only"
