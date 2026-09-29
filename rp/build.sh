#!/bin/bash
set -euo pipefail

# Down to main path
cd ..

# Clear stale git submodule index locks left behind by interrupted prior
# runs ("set -e" + git fatals + nested submodules occasionally leave
# .git/modules/*/index.lock around, which blocks every subsequent
# `git submodule update`). Safe to do unconditionally because we're not
# running any concurrent git operations.
find .git -name "index.lock" -delete 2>/dev/null || true

# Install SDK needed for building
git submodule init
git submodule update --init --recursive

# Pin the building versions
echo "Pinning the SDK versions..."
cd pico-sdk
git checkout tags/2.2.0
cd ..

echo "Pinning the Extras SDK versions..."
cd pico-extras
git checkout tags/sdk-2.2.0
cd ..

echo "Pinning the FatFs SDK versions..."
cd fatfs-sdk
# v3.6.2 releases the card and SPI locks when a read fails: without it, the
# second disk call after the card is pulled waits forever.
git checkout tags/v3.6.2
cd ..

# FatFs configuration is overridden by rp/src/ff/ffconf.h; the CMake
# include path puts that directory ahead of the submodule's default copy
# so we no longer need to sed-patch the submodule on every build (which
# left fatfs-sdk dirty and unrecordable in the parent commit).

# Set the environment variables of the SDKs
export PICO_SDK_PATH=$PWD/pico-sdk
export FATFS_SDK_PATH=$PWD/fatfs-sdk
export PICO_EXTRAS_PATH=$PWD/pico-extras

# Return to rp path
cd rp

# Check if the third parameter is provided
export RELEASE_TYPE=${3:-""}
echo "Release type: $RELEASE_TYPE"

# Determine the file to use based on RELEASE_TYPE
if [ -z "$RELEASE_TYPE" ] || [ "$RELEASE_TYPE" = "final" ]; then
    VERSION_FILE="version.txt"
else
    VERSION_FILE="version-$RELEASE_TYPE.txt"
fi

# Read the release version from the version.txt file. Assigned before it is
# exported: `export VAR=$(...)` would hide a failed read.
RELEASE_VERSION=$(cat "$VERSION_FILE" | tr -d '\r\n ')
export RELEASE_VERSION
echo "Release version: $RELEASE_VERSION"

# Get the release date and time from the current date, unless the caller set
# one: a fixed RELEASE_DATE makes two builds of the same commit byte-identical.
export RELEASE_DATE=${RELEASE_DATE:-$(date +"%Y-%m-%d %H:%M:%S")}
echo "Release date: $RELEASE_DATE"

# Set the board type to be used for building
# If nothing passed as first argument, use pico_w
export BOARD_TYPE=${1:-pico_w}
export PICO_BOARD=$BOARD_TYPE
echo "Board type: $BOARD_TYPE"

# Build type, case-insensitive. If nothing is passed, use release. Both are
# CMake Release (-O3); debug is the same build with DEBUG_MODE=1, so DPRINTF
# traces go to the UART console.
BUILD_TYPE=$(echo "${2:-release}" | tr '[:upper:]' '[:lower:]')
case "$BUILD_TYPE" in
    release|debug) ;;
    *)
        echo "ERROR: unknown build type '$2'. Use release or debug."
        exit 1
        ;;
esac
export BUILD_TYPE
echo "Build type: $BUILD_TYPE"

# Translate (board, build_type) into a CMakePresets.json preset name. The
# preset's `environment` block sets PICO_BOARD + DEBUG_MODE for CMake, so
# the script's exports above are redundant when using presets -- left in
# place for parity with the env CMakeLists also reads (RELEASE_VERSION,
# RELEASE_DATE).
if [ "$BUILD_TYPE" = "release" ]; then
    export DEBUG_MODE=0
    PRESET_KIND="release"
else
    export DEBUG_MODE=1
    PRESET_KIND="debug"
fi

CONFIGURE_PRESET="${BOARD_TYPE}-${PRESET_KIND}"
BUILD_PRESET="${BOARD_TYPE}-${PRESET_KIND}"
BUILD_DIR="build-${BOARD_TYPE}-${PRESET_KIND}"

# Up to v1.0.0beta every build was compiled MinSizeRel, because a Release
# build once broke at runtime (no reproducer was recorded). The presets set
# Release; RP_CMAKE_BUILD_TYPE replaces only the CMake build type: MinSizeRel
# to compare against those builds, Debug (-Og, asserts on) to step through the
# code in a debugger. The build ID carries it (+minsizerel, +cmakedebug).
CMAKE_BUILD_TYPE_ARG=Release
if [ -n "${RP_CMAKE_BUILD_TYPE:-}" ]; then
    CMAKE_BUILD_TYPE_ARG=$RP_CMAKE_BUILD_TYPE
    echo "************************************************************"
    echo "WARNING: RP_CMAKE_BUILD_TYPE=$RP_CMAKE_BUILD_TYPE overrides the"
    echo "         CMake build type. This is not a shipping build."
    echo "************************************************************"
fi
echo "CMake build type: $CMAKE_BUILD_TYPE_ARG (DEBUG_MODE=$DEBUG_MODE)"
echo "Configure preset: $CONFIGURE_PRESET"
echo "Build preset: $BUILD_PRESET"

# Clean only the preset-specific build directory so debug + release
# builds can coexist on disk.
echo "Deleting previous preset build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"

# CMakePresets.json lives in rp/src/ -- configure + build are run from
# there.
(
    cd src
    cmake --preset "$CONFIGURE_PRESET" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE_ARG"
    cmake --build --preset "$BUILD_PRESET"
)

# Copy the built firmware to the /dist folder
mkdir -p dist
echo "Copying the built firmware to the dist folder"
if [ "$BUILD_TYPE" = "release" ]; then
    cp "$BUILD_DIR/rp.uf2" "dist/rp-$BOARD_TYPE.uf2"
else
    cp "$BUILD_DIR/rp.uf2" "dist/rp-$BOARD_TYPE-$BUILD_TYPE.uf2"
fi
