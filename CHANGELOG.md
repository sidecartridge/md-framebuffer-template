# Changelog

## v1.2.1 (2026-05-20) - release

### Sync ack: no false positives on missing hardware

The m68k `send_sync` / `send_sync_write` waiters previously ack'd a
command as soon as `RANDOM_TOKEN == d2` (the token the m68k sent).
That condition also holds when the cartridge is unplugged or the
microfirmware is not running, because the bus returns the same
open-bus / uninitialised value at both `RANDOM_TOKEN_ADDR` and
`RANDOM_TOKEN_SEED_ADDR` — apps would proceed as if the device had
replied.

The waiter now additionally requires `RANDOM_TOKEN_SEED_ADDR` to have
moved away from the value the m68k loaded at request time. The RP
side already writes a strictly-incrementing `incrementalCmdCount` to
the seed slot on every response, so a real reply always advances the
seed; absent hardware leaves it equal to `d2` and the loop runs to
timeout instead.

RP-side `chandler.c` gains a comment documenting the
SEED-must-advance invariant that the m68k waiter depends on.

Thanks to @neilrackett (PR #5) for the fix.

---

## v1.2.0 (2026-04-28) - release

### Shared 64 KB region rearranged

Single source-of-truth layout for the region mirrored at m68k
`$FA0000` / RP `0x20030000`. Cartridge code lives in the first 8 KB,
metadata block sits just above it, and the framebuffer moves to the
top so app data fills one contiguous 48 KB arena.

| m68k addr | Region                                       |
| --------- | -------------------------------------------- |
| `$FA0000` | CARTRIDGE (max 8 KB, build.sh-enforced)      |
| `$FA2000` | sentinel + random token + 60 shared vars     |
| `$FA2100` | TRANSTABLE (512 B high-res mask table)       |
| `$FA2300` | APP_FREE (~48 KB)                            |
| `$FAE0C0` | FRAMEBUFFER (8000 B)                         |

Reference offsets via the constants in `rp/src/include/chandler.h`
and `target/atarist/src/main.s`. Apps that hard-coded the old
addresses (`$FA8000` framebuffer, `$FAF000` random token) must
migrate.

### User firmware module

New per-module split via `target/atarist/src/userfw.ld`:
2 KB for `main.s` + 6 KB for the new `userfw.s` (entry at
`USERFW = $FA0800`). Pattern mirrors md-drives-emulator's
`gemdrive.ld`.

Launch path: RP terminal menu `[F]irmware` → `CMD_START = 4` on the
cartridge sentinel → `rom_function: jmp USERFW`. Default `userfw.s`
is a Cconws demo that clears the screen and prints
`Example firmware load...`.

---

## v1.1.0 (2026-04-27) - release

Architectural port of the framework improvements introduced in
md-drives-emulator. Apps derived from previous versions of this template
will need to migrate (see "Breaking changes" below).

### Memory layout
- RAM grown from 128 KB to 192 KB.
- ROM_IN_RAM reduced from 128 KB @ `0x20020000` to 64 KB @ `0x20030000`.
- Result: 64 KB of additional general-purpose RAM available to apps.

### ROM4 read engine (cartridge data path)
- Single-bank 64 KB ROM (ROM_BANKS 2 → 1). ROM3 is no longer a data
  bank; it is now used exclusively as the command channel.
- PIO program rewritten: waits directly on `ROM4_GPIO`, captures 16
  bits of address, two-channel chained DMA serves reads with no CPU
  or IRQ involvement.
- `ROMEMUL_BUS_BITS` 17 → 16; `FLASH_ROM3_LOAD_OFFSET` removed.

### ROM3 command channel — new (`commemul`)
- Dedicated PIO state machine on `ROM3_GPIO` captures every ROM3
  access into a 32 KB ring buffer via DMA in ring mode (no IRQ).
- `commemul_poll(callback)` drains the ring lock-free using
  `dma_hw->ch[ch].transfer_count` to derive the producer index.

### Command dispatcher — new (`chandler`)
- Polled command parser/dispatcher. `chandler_loop()` calls
  `commemul_poll`, parses via `tprotocol_parse`, and dispatches each
  command to a registered callback list.
- Apps register handlers with `chandler_addCB(callback)`.
- Replaces the previous `DMA_IRQ_1`-driven snoop in `term.c`.

### Terminal
- Removed `term_dma_irq_handler_lookup`. Terminal commands are now
  delivered through `term_command_cb`, which is registered with
  `chandler_addCB` during emulation startup.

### Orchestration (emul.c)
- Boot now calls
  `init_romemul(false); commemul_init(); chandler_init();
  chandler_addCB(term_command_cb);`.
- Main loop drains commands via `chandler_loop()` before `term_loop()`.
- WiFi polling callback drains both, so commands sent during the
  multi-second WiFi connect window are not dropped.

### m68k framework fixes
- `inc/tos.s`: fix `endmv` → `endm` typo in the `pchar2` macro.
- `inc/sidecart_functions.s`: rename transient `d0` → `d4` in the
  `_write_to_sidecart_*` loops. `d0` is the sync command reply
  register; reusing it mid-write clobbered the reply slot for any
  app that issued payload writes.
- New `COMMAND_WRITE_TIMEOUT` (defaults to `COMMAND_TIMEOUT`) used by
  `_start_sync_write_code_in_stack`, so apps can extend the write
  timeout for large payloads without affecting read-command timing.

### Breaking changes for downstream apps
- `init_romemul(IRQInterceptionCallback, IRQInterceptionCallback,
  bool)` → `init_romemul(bool)`.
- Removed: `dma_setResponseCB`, `romemul_getLookupDataRomDmaChannel`,
  `dma_irqHandlerLookup`, `dma_irqHandlerAddress`, the
  `IRQInterceptionCallback` typedef.
- `tprotocol`: `TransmissionProtocol.payload` is now `uint16_t[/2]`
  (was `unsigned char[]`). Static parser state is now extern; the
  template provides `tprotocol.c` defining the externs.
- `term.h`: `ADDRESS_HIGH_BIT` and the `ROM3_GPIO` define removed.

### Build
- Added `chandler.c`, `commemul.c` to `rp/src/CMakeLists.txt` sources.
- Added `pico_generate_pio_header` for `commemul.pio`.

---

## v0.0.3 (2025-07-01) - release
- First version

---
