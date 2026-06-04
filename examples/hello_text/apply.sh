#!/usr/bin/env bash
#
# examples/hello_text/apply.sh
#
# Back up rp/ to rp.bak, then customize rp/ to build the hello_text
# example: remove the bundled demos and install this example's emul.c +
# CMakeLists.txt. Run it from anywhere in a template checkout.
#
# Revert with:   rm -rf rp && mv rp.bak rp
#
set -euo pipefail

# Resolve the repo root from this script's own location (examples/hello_text).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

if [ ! -d rp ]; then
  echo "error: no rp/ directory at $ROOT -- run from a template checkout." >&2
  exit 1
fi
if [ -e rp.bak ]; then
  echo "error: rp.bak already exists -- refusing to overwrite the backup." >&2
  echo "       restore (rm -rf rp && mv rp.bak rp) or remove rp.bak first." >&2
  exit 1
fi

echo "Backing up rp/ -> rp.bak ..."
cp -a rp rp.bak

echo "Removing demo sources + asset headers ..."
rm -f rp/src/demo_menu.c rp/src/demo_parallax.c rp/src/demo_3d.c \
      rp/src/demo_sprites.c rp/src/demo_cojorotozoom.c
rm -f rp/src/include/demo.h \
      rp/src/include/sidecart_logo.h rp/src/include/sidecart_text.h \
      rp/src/include/solid3d.h rp/src/include/sprites_data.h \
      rp/src/include/cojo_texture.h rp/src/include/cojo_font.h \
      rp/src/include/diego_sprite.h rp/src/include/uridium_surface.h

echo "Installing the hello_text app (emul.c + CMakeLists.txt) ..."
cp "$SCRIPT_DIR/emul.c" rp/src/emul.c
cp "$SCRIPT_DIR/CMakeLists.txt" rp/src/CMakeLists.txt

cat <<'EOF'

Done. rp/ now builds the hello_text example; the original is in rp.bak.

  Build:   ./build.sh pico_w release 44444444-4444-4444-8444-444444444444
  Revert:  rm -rf rp && mv rp.bak rp
EOF
