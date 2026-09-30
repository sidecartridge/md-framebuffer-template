#!/bin/bash
# Build the RP firmware out of tree, flash it, and confirm the RP runs it.
#
#   tools/dev/flash.sh <release|debug> [--src DIR] [--build-only] [--probe]
#                      [--timeout S]
#
# Builds into tools/dev/builds/<type> (incremental; never touches rp/build-* or
# the submodules) with the settings of the pico_w-<type> preset in
# rp/src/CMakePresets.json, keeps rp.elf as
# tools/dev/builds/elf/<type>-<build id>.elf, and flashes with picotool (or the
# Debug Probe when picotool cannot see the RP, or with --probe). Then checks
# over SWD, without the firmware's help, that the RP booted the ELF, that its
# flash matches the ELF and that it carries the new build ID
# (tools/dev/swd.py). It does not build the m68k image: after a change in
# target/atarist/, run target/atarist/build.sh first (it regenerates
# rp/src/include/target_firmware.h).
#
# Environment: APP_UUID_KEY (default: the development UUID), OPENOCD and
# PICO_OPENOCD_PATH (see swd.py), RELEASE_DATE (default: the date of the HEAD
# commit, so builds of one commit are byte-identical), RP_CMAKE_BUILD_TYPE for
# a CMake type other than Release (as rp/build.sh).
set -Eeo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

usage() { sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }

TYPE="${1:-}"
case "$TYPE" in
  release) DEBUG_MODE=0 ;;
  debug) DEBUG_MODE=1 ;;
  *) usage ;;
esac
shift
SRC="$REPO/rp/src"
BUILD_ONLY=0
USE_PROBE=0
TIMEOUT=30
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$(cd "$2" && pwd)"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --probe) USE_PROBE=1; shift ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

NAME="$TYPE"
# Another CMake type (RP_CMAKE_BUILD_TYPE) builds in its own folder, and so
# does the 25 fps profile (APP_PROFILE, rp/src/include/profile.h).
if [ -n "${RP_CMAKE_BUILD_TYPE:-}" ]; then
  NAME="$NAME-$(printf '%s' "$RP_CMAKE_BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
fi
if [ "${APP_PROFILE:-}" = "PROFILE_25FPS" ]; then
  NAME="$NAME-25fps"
fi
# A source outside the repo gets its own build folder, keyed by its path: two
# checkouts are both called rp/src, and sharing a folder confuses CMake's cache.
if [ "$SRC" != "$REPO/rp/src" ]; then
  NAME="$NAME-$(basename "$(dirname "$(dirname "$SRC")")")-$(printf '%s' "$SRC" | shasum | cut -c1-6)"
fi
OUT="$HERE/builds/$NAME"
mkdir -p "$OUT" "$HERE/builds/elf"

# Same environment as rp/build.sh, without its submodule checkout: this script
# is for fast iteration and the pins are only verified, never changed.
for pin in "pico-sdk tags/2.2.0" "pico-extras tags/sdk-2.2.0" \
           "fatfs-sdk tags/v3.6.2"; do
  set -- $pin
  if [ "$(git -C "$REPO/$1" rev-parse HEAD)" != "$(git -C "$REPO/$1" rev-parse "$2^{commit}")" ]; then
    echo "WARNING: $1 is not at $2; run rp/build.sh once to pin it" >&2
  fi
done

# The m68k image is embedded as rp/src/include/target_firmware.h, which only
# target/atarist/build.sh regenerates.
FIRMWARE_H="$SRC/include/target_firmware.h"
M68K="$(dirname "$(dirname "$SRC")")/target/atarist"
if [ -d "$M68K" ] && [ -n "$(find "$M68K/src" "$M68K/Makefile" -newer "$FIRMWARE_H" \
     \( -name '*.s' -o -name '*.ld' -o -name Makefile \) 2>/dev/null | head -1)" ]; then
  echo "WARNING: target/atarist/ changed after target_firmware.h was made:" \
       "run target/atarist/build.sh first if the ST side changed" >&2
fi

export PICO_SDK_PATH="$REPO/pico-sdk" PICO_EXTRAS_PATH="$REPO/pico-extras" \
       FATFS_SDK_PATH="$REPO/fatfs-sdk"
export BOARD_TYPE=pico_w PICO_BOARD=pico_w
export APP_UUID_KEY="${APP_UUID_KEY:-44444444-4444-4444-8444-444444444444}"
RELEASE_VERSION="$(tr -d '\r\n ' < "$REPO/version.txt")"
export RELEASE_VERSION RELEASE_TYPE="${RELEASE_TYPE:-}" DEBUG_MODE
if [ -z "${RELEASE_DATE:-}" ]; then
  RELEASE_DATE="$(git -C "$REPO" log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M:%S')"
fi
export RELEASE_DATE

echo "Building $NAME from $SRC"
# The preset's settings (CMake Release, DEBUG_MODE) in our own folder, unless
# RP_CMAKE_BUILD_TYPE says otherwise (MinSizeRel to compare, Debug to step
# through the code).
if [ -n "${RP_CMAKE_BUILD_TYPE:-}" ]; then
  echo "WARNING: RP_CMAKE_BUILD_TYPE=$RP_CMAKE_BUILD_TYPE: not a shipping build"
fi
(cd "$SRC" && cmake --preset "pico_w-$TYPE" -B "$OUT" \
   -DCMAKE_BUILD_TYPE="${RP_CMAKE_BUILD_TYPE:-Release}") > "$OUT/cmake.log" 2>&1 \
  || { tail -20 "$OUT/cmake.log"; exit 1; }
cmake --build "$OUT" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" > "$OUT/make.log" 2>&1 \
  || { grep -n 'error' "$OUT/make.log" | head -30; exit 1; }

BUILD_ID="$(sed -n 's/^#define RELEASE_BUILD_ID "\(.*\)"$/\1/p' \
  "$OUT/generated/build_id/build_id.h" 2> /dev/null || true)"
BUILD_ID="${BUILD_ID:-no-build-id}"
cp "$OUT/rp.elf" "$HERE/builds/elf/$TYPE-$BUILD_ID.elf"
arm-none-eabi-size "$OUT/rp.elf" | tail -1
echo "Built $TYPE $BUILD_ID: $OUT/rp.uf2"
[ "$BUILD_ONLY" = 1 ] && exit 0

if [ "$USE_PROBE" = 1 ]; then
  python3 "$HERE/swd.py" program "$OUT/rp.elf"
elif picotool load -f -x "$OUT/rp.uf2" > "$OUT/picotool.log" 2>&1; then
  echo "Flashed with picotool"
else
  echo "picotool could not flash ($(grep -m1 -i 'no accessible\|error' "$OUT/picotool.log" || echo 'see picotool.log')); using the Debug Probe"
  python3 "$HERE/swd.py" program "$OUT/rp.elf"
fi

# Verified over SWD, without the firmware's help: the RP has left the bootrom
# for this ELF, its flash matches the ELF byte for byte, and the build ID is
# the one just built.
if python3 "$HERE/swd.py" running "$OUT/rp.elf" --timeout "$TIMEOUT" \
   && python3 "$HERE/swd.py" verify "$OUT/rp.elf" \
   && { [ "$BUILD_ID" = no-build-id ] \
        || [ "$(python3 "$HERE/swd.py" build-id "$OUT/rp.elf")" = "$BUILD_ID" ]; }; then
  echo "Running $TYPE $BUILD_ID"
  exit 0
fi
echo "ERROR: the RP is not running $TYPE $BUILD_ID" >&2
if [ -f "$HERE/logs/console.log" ]; then
  echo "--- console since the last boot (last 30 lines)" >&2
  python3 "$HERE/console.py" since-boot 2>/dev/null | tail -30 >&2 || true
fi
exit 1
