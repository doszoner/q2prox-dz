#!/usr/bin/env bash
set -euo pipefail
mode="portable"
case "${1:-}" in
  "") ;;
  --portable) mode="portable" ;;
  --live) mode="live" ;;
  *) echo "usage: doctor.sh [--portable|--live]" >&2; exit 64 ;;
esac

fail=0
check_cmd() {
  if command -v "$1" >/dev/null 2>&1; then
    printf 'PASS %-20s %s\n' "$1" "$(command -v "$1")"
  else
    printf 'FAIL %-20s missing\n' "$1"
    fail=1
  fi
}
for cmd in cmake ninja c++ python3 sha256sum readelf file; do check_cmd "$cmd"; done

if [[ "$mode" == live ]]; then
  if command -v wine >/dev/null 2>&1; then check_cmd wine
  elif command -v wine64 >/dev/null 2>&1; then check_cmd wine64
  else printf 'FAIL %-20s missing wine/wine64\n' wine; fail=1
  fi
  if [[ -n "${WINEPREFIX:-}" ]]; then
    [[ -d "$WINEPREFIX" ]] && printf 'PASS WINEPREFIX           %s\n' "$WINEPREFIX" || {
      printf 'FAIL WINEPREFIX           missing: %s\n' "$WINEPREFIX"; fail=1;
    }
  else
    printf 'INFO WINEPREFIX           not set; Wine default will be used\n'
  fi
else
  printf 'INFO mode                 portable; Wine is checked by --live\n'
fi

for optional in shellcheck cppcheck; do
  if command -v "$optional" >/dev/null 2>&1; then
    printf 'INFO %-20s %s\n' "$optional" "$(command -v "$optional")"
  else
    printf 'INFO %-20s not installed; optional local lint gate\n' "$optional"
  fi
done
exit "$fail"
