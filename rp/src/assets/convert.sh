#!/usr/bin/env bash
#
# The games' art (game_arena.c, game_zap.c): indexed PNGs sharing one
# 16-colour palette, converted to headers in rp/src/include by
# tools/png_to_bitmap.py. Run after editing a PNG.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
TOOL=../../../tools/png_to_bitmap.py
INC=../include

python3 "$TOOL" tiles.png --out "$INC/art_tiles.h" --tile 16 16   # two floors, a wall
python3 "$TOOL" player.png --out "$INC/art_player.h"
python3 "$TOOL" enemy.png --out "$INC/art_enemy.h" --tile 16 16   # two frames
python3 "$TOOL" gem.png --out "$INC/art_gem.h" --tile 16 16       # two frames
python3 "$TOOL" bolt.png --out "$INC/art_bolt.h"
python3 "$TOOL" crosshair.png --out "$INC/art_crosshair.h"
