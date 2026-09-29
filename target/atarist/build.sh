#!/bin/bash

# Fail fast. Without this a failed step was stepped over and the RP build went
# on to embed the committed target_firmware.h, which no longer matches these
# sources. `-u` is not set: the argument checks test unset arguments.
set -Eeo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

# Ensure an argument is provided
if [ -z "$1" ]; then
    echo "Usage: $0 <working_folder> all|release"
    exit 1
fi

if [ -z "$2" ]; then
    echo "Usage: $0 <working_folder> all|release"
    exit 1
fi

working_folder=$1
build_type=$2
target_firmware="target_firmware.h"

# (fbdrv.s used to be regenerated here by gen_fbdrv.py before the
# m68k make. That step is gone: the cart->ST copy is now the
# FBDRV_INLINE macro inside userfw.s.)

# ST_WORKING_FOLDER=$working_folder/configurator stcmd make $build_type
# STCMD_NO_TTY=1 keeps docker working when invoked from non-TTY contexts
# (CI, sub-shells, build wrappers). Without it stcmd's `-it` flag aborts
# with "the input device is not a TTY" and the build silently keeps
# whatever BOOT.BIN was previously generated.
make_status=0
STCMD_NO_TTY=1 ST_WORKING_FOLDER=$working_folder stcmd make $build_type || make_status=$?
if [ "$make_status" -ne 0 ]; then
    echo "ERROR: m68k make failed (status $make_status)"
    exit $make_status
fi

# Cartridge code budget: header + code + fbdrv must fit in 16 KB
# (CART_CARTRIDGE_CODE_SIZE in rp/src/include/cart_shared.h, mirrored as
# CARTRIDGE_CODE_SIZE in target/atarist/src/main.s). Bumped from 8 KB to
# 16 KB to make room for the unrolled MOVEM blit (now the FBDRV_INLINE
# macro in userfw.s). Enforce here so the build fails fast instead of silently
# overlapping the shared block.
# stat directly on the host to avoid the stcmd banner contaminating stdout.
boot_bin="$working_folder/dist/BOOT.BIN"
cartridge_max=16384
if [ ! -f "$boot_bin" ]; then
    echo "ERROR: $boot_bin not produced by stcmd make"
    exit 4
fi
if [ "$(uname)" = "Darwin" ]; then
    boot_size=$(stat -f %z "$boot_bin")
else
    boot_size=$(stat -c %s "$boot_bin")
fi
if [ "$boot_size" -gt "$cartridge_max" ]; then
    echo "ERROR: cartridge code is $boot_size bytes; limit is $cartridge_max"
    echo "       (CART_CARTRIDGE_CODE_SIZE in cart_shared.h /"
    echo "        CARTRIDGE_CODE_SIZE in main.s)"
    exit 5
fi
echo "Cartridge code: $boot_size / $cartridge_max bytes"

#filename_tos="./dist/SIDECART.TOS"

# Copy the SIDECART.TOS file for testing purposes
#ST_WORKING_FOLDER=$working_folder stcmd cp ./configurator/dist/SIDECART.TOS $filename_tos

filename="./dist/FIRMWARE.IMG"

# Copy the BOOT.BIN file to a ROM size file for testing
STCMD_NO_TTY=1 ST_WORKING_FOLDER=$working_folder stcmd cp ./dist/BOOT.BIN $filename

# Determine the file size accordingly
if [ "$(uname)" = "Darwin" ]; then
    filesize=$(stat -f %z "$working_folder/${filename#./}")
else
    filesize=$(stat -c %s "$working_folder/${filename#./}")
fi

# Size for 64Kbytes in bytes
targetsize=$((64 * 1024))

# Check if the file is larger than 64Kbytes
if [ "$filesize" -gt "$targetsize" ]; then
    echo "The file is already larger than 64Kbytes."
    exit 2
fi

# Resize the file to 64Kbytes
if ! STCMD_NO_TTY=1 ST_WORKING_FOLDER=$working_folder stcmd truncate -s $targetsize $filename; then
    echo "Failed to resize the file."
    exit 3
fi

echo "File has been resized."

echo "Creating the firmware.h file."
# Remove any header left by an earlier run, so the check below proves that
# this run generated it. python3 first: stock macOS has no `python`.
rm -f "$target_firmware"
python_bin=$(command -v python3 || command -v python)
"$python_bin" firmware.py --input=dist/FIRMWARE.IMG --output=$target_firmware --array_name=target_firmware
if [ ! -s "$target_firmware" ]; then
    echo "ERROR: firmware.py did not produce $target_firmware"
    exit 6
fi

cp $target_firmware ../../rp/src/include/$target_firmware
echo "Copied $target_firmware to rp/src/include/$target_firmware"

rm $target_firmware
echo "Removed $target_firmware"
