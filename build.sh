#!/bin/bash

# Fail fast. Without this a failed step was stepped over: the RP build went on
# with the committed target_firmware.h, or build.sh copied a UF2 an earlier run
# had left in rp/dist, and still exited 0 with a JSON announcing the new
# version. `-u` is not set: the argument checks below test unset arguments.
set -Eeo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

# Get the absolute path of the current script
SCRIPT_DIR=$(dirname "$(realpath "$0")")

# Ensure all required arguments are provided
if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
    echo "Usage: $0 <board_type> <build_type> <app_uuid_key>"
    echo "Example: $0 pico_w release|debug 123e4567-e89b-12d3-a456-426614174000"
    exit 1
fi

# Set the dist directory. Delete previous contents first, so a build that
# fails at any later step leaves no UF2 or JSON behind
echo "Delete previous dist directory"
rm -rf dist
mkdir dist

# Copy the version.txt to each project
echo "Copy version.txt to each project"
cp version.txt rp/
cp version.txt target/

# Display the version information. Not exported: the stcmd written by older
# atarist-toolkit-docker installers (v1.0.0) uses $VERSION as its Docker
# image tag, and there is no image for the firmware's version.
VERSION=$(cat version.txt)
echo "Version: $VERSION"

# Set the board type to be used for building
export BOARD_TYPE=$1
echo "Board type: $BOARD_TYPE"

# Build type, case-insensitive. debug is the same build with DPRINTF traces on
# the UART console, on the RP, and _DEBUG=1 in the m68k assembly.
BUILD_TYPE=$(echo "$2" | tr '[:upper:]' '[:lower:]')
case "$BUILD_TYPE" in
    release) DEBUG_MODE=0 ;;
    debug) DEBUG_MODE=1 ;;
    *)
        echo "ERROR: unknown build type '$2'. Use release or debug."
        exit 1
        ;;
esac
export BUILD_TYPE DEBUG_MODE
echo "Build type: $BUILD_TYPE (DEBUG_MODE=$DEBUG_MODE)"

# One date for both images, unless the caller sets one: with a fixed
# RELEASE_DATE two builds of one commit are byte-identical.
export RELEASE_DATE=${RELEASE_DATE:-$(date +"%Y-%m-%d %H:%M:%S")}
echo "Release date: $RELEASE_DATE"

# Set the APP_UUID_KEY of the app to be built
export APP_UUID_KEY=$3
echo "App UUID Key: $APP_UUID_KEY"

# Build the project in the target architecture. target/atarist/build.sh
# regenerates rp/src/include/target_firmware.h and fails if it cannot.
echo "Building target project"
(cd target/atarist && ./build.sh "$SCRIPT_DIR/target/atarist" release "$DEBUG_MODE")
echo "Done building target project"

# Build the rp project in the RP architecture
echo "Building rp project"
(cd rp && ./build.sh "$BOARD_TYPE" "$BUILD_TYPE")
if [ "$BUILD_TYPE" = "release" ]; then
    cp "rp/dist/rp-$BOARD_TYPE.uf2" dist/rp.uf2
else
    cp "rp/dist/rp-$BOARD_TYPE-$BUILD_TYPE.uf2" dist/rp.uf2
fi
echo "Done building rp project"

# Fail loudly if the RP firmware wasn't produced, instead of continuing
# and leaving an empty dist/ for downstream steps (e.g. the CI release job,
# which then fails on a confusing `cp dist/*.uf2`).
if [ ! -f dist/rp.uf2 ]; then
    echo "ERROR: RP firmware not produced (dist/rp.uf2 missing). Aborting."
    exit 1
fi

# Calculate the md5sum of the generated rp.uf2 file. Stock macOS has md5, not
# md5sum.
if command -v md5sum >/dev/null 2>&1; then
    MD5_HASH=$(md5sum dist/rp.uf2 | cut -d ' ' -f 1)
else
    MD5_HASH=$(md5 -q dist/rp.uf2)
fi
echo "$MD5_HASH  dist/rp.uf2" > dist/rp.uf2.md5sum

# Show the md5sum of the generated rp.uf2 file
echo "md5sum of the generated rp.uf2 file:"
cat dist/rp.uf2.md5sum

# Now inform the user that the build is complet and must
# modify the app.json file with the new md5sum and the UUID
echo "Build completed successfully. Please update the app.json file with the new md5sum and the UUID"

# Rename the file to the standard name <APP_UUID>.uf2
mv dist/rp.uf2 dist/$APP_UUID_KEY.uf2

# Check that there is a app.json file in the dist directory
if [ ! -f desc/app.json ]; then
    echo "app.json file not found in the 'desc'' directory. Please create one."
    exit 1
fi

# Copy the app.json file to the dist directory
cp desc/app.json dist/

# Use portable sed for Linux and macOS
if [ "$(uname)" = "Darwin" ]; then
    sed -i '' "s/<APP_UUID>/$APP_UUID_KEY/g" dist/app.json
    sed -i '' "s/<BINARY_MD5_HASH>/$MD5_HASH/g" dist/app.json
    sed -i '' "s/<APP_VERSION>/$VERSION/g" dist/app.json
else
    sed -i "s/<APP_UUID>/$APP_UUID_KEY/g" dist/app.json
    sed -i "s/<BINARY_MD5_HASH>/$MD5_HASH/g" dist/app.json
    sed -i "s/<APP_VERSION>/$VERSION/g" dist/app.json
fi

mv dist/$APP_UUID_KEY.uf2 dist/$APP_UUID_KEY-$VERSION.uf2

# Show the content of the $APP_UUID_KEY.json file
echo "Content of the $APP_UUID_KEY.json file:"
mv dist/app.json dist/$APP_UUID_KEY.json
cat dist/$APP_UUID_KEY.json

# Done
exit 0
