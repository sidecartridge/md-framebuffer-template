# SidecarTridge Multi-device Microfirmware App template

This is the template to create a Microfirmware app for the SidecarTridge Multidevice-app for Atari ST computers.

# ⚠️ ATTENTION! READ THIS FIRST

The process for creating a microfirmware app from this template is now documented in the official [SidecarTridge Multi-device documentation](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/). To avoid inconsistencies and outdated information, we've centralized the instructions there. Please refer to the official documentation for the latest guidance.

## Shared 64 KB region layout

The template now ships with a single source-of-truth layout for the 64 KB shared region (m68k `$FA0000`–`$FAFFFF`, mirrored at RP `0x20030000`):

- The cartridge image (m68k header + code) lives in the first **8 KB** (`$FA0000`–`$FA1FFF`). `target/atarist/build.sh` enforces this with a hard size check on `BOOT.BIN`.
- A small fixed-offset metadata block (`CMD_MAGIC_SENTINEL`, `RANDOM_TOKEN`, `RANDOM_TOKEN_SEED`, 60 × 4-byte indexed shared variables) sits at `$FA2000`.
- The **APP_FREE** arena (~48 KB at `$FA2300`) is the contiguous space your app should use for its own buffers.
- The **framebuffer** (8000 B for 320×200 monochrome) sits at the very top of the region (`$FAE0C0`), so an overrun walks off the end of the 64 KB window instead of corrupting the metadata block.

Both sides derive every offset symbolically from the constants in `rp/src/include/chandler.h` (RP-side) and `target/atarist/src/main.s` (m68k side). Apps must never hard-code an address inside the region — always reference the named offset/symbol so the layout stays the single source of truth.

See `programming.md` for the full table and the budget rules.

## User firmware module

The cartridge image is split via `target/atarist/src/userfw.ld` into two sections:

- `main.s` at offset `0x0000` (`$FA0000`, 2 KB) — boot, dispatch, terminal.
- `userfw.s` at offset `0x0800` (`$FA0800`, 6 KB) — your app-specific m68k code.

`main.s` exposes the user firmware as `USERFW equ (ROM4_ADDR + $800)`. When the RP-side terminal command `f` (`[F]irmware`) is selected, the RP writes `CMD_START = 4` to the cartridge sentinel; the m68k's vsync-polled `check_commands` dispatches to `rom_function`, which `jmp`s to `USERFW`. The default `userfw.s` ships with a Cconws demo that prints `Example firmware load...` to the screen — replace the body with your own logic.

Adding more modules follows the same `gemdrive.ld`-style pattern used by `md-drives-emulator`: place each new `.text` section in `userfw.ld`, mirror the offset with an `equ` in `main.s`, and add the `.o` target to `target/atarist/Makefile`.

## License

The source code of the project is licensed under the GNU General Public License v3.0. The full license is accessible in the [LICENSE](LICENSE) file. 
