#!/bin/bash
# Out-of-tree measurement builds of the rp/ firmware: MinSizeRel and Release,
# each with and without DEBUG_MODE, and CMake Debug.
#
# Does not touch rp/build-*, rp/dist or the submodule pins (it never runs
# rp/build.sh, which re-pins submodules). Adds -fstack-usage and
# -fcallgraph-info=su so tools/dev/stackdepth.py can compute worst-case stack
# depth.
#
# Usage: tools/dev/measure_builds.sh [OUT_DIR]
#
# Flags go in through CFLAGS on purpose: passing -DCMAKE_C_FLAGS replaces
# the Pico toolchain's -mcpu=cortex-m0plus -mthumb and every file fails to
# assemble.
set -Eeo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"

export PICO_SDK_PATH="$REPO/pico-sdk"
export PICO_EXTRAS_PATH="$REPO/pico-extras"
export FATFS_SDK_PATH="$REPO/fatfs-sdk"
export BOARD_TYPE=pico_w
export PICO_BOARD=pico_w
export APP_UUID_KEY=44444444-4444-4444-8444-444444444444
export RELEASE_VERSION="$(tr -d '\r\n ' < "$REPO/version.txt")"
export RELEASE_DATE="measurement"
export RELEASE_TYPE=final

build() {
  local name=$1 cmtype=$2 dbg=$3
  local bdir="$OUT/build-$name"
  rm -rf "$bdir"; mkdir -p "$bdir"
  echo "=== [$name] CMAKE_BUILD_TYPE=$cmtype DEBUG_MODE=$dbg ==="
  ( cd "$bdir" \
    && DEBUG_MODE=$dbg CFLAGS="-fstack-usage -fcallgraph-info=su" \
       cmake "$REPO/rp/src" -DCMAKE_BUILD_TYPE="$cmtype" > cmake.log 2>&1 \
    && DEBUG_MODE=$dbg make -j8 > make.log 2>&1 ) || { echo "[$name] FAILED, see $bdir/make.log"; return 1; }
  arm-none-eabi-size -A "$bdir/rp.elf" | grep -E '^\.(text|rodata|data|bss)\s'
  local bss_end
  bss_end=$(grep '__bss_end__ = \.' "$bdir/rp.elf.map" | awk '{print $1}')
  printf "heap (bss end to 0x20030000, the cartridge window): %d bytes\n" $((0x20030000 - bss_end))
}

build minsizerel-rel MinSizeRel 0
build minsizerel-dbg MinSizeRel 1
build release        Release    0
build release-dbg    Release    1
build debug          Debug      1
echo "Outputs in $OUT"
