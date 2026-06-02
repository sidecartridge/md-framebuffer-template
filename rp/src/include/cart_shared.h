/**
 * File: cart_shared.h
 * Description: Cart 64 KB shared-region layout + cart-bus helpers.
 *
 * The cart shared region at $FA0000..$FAFFFF on the m68k mirrors RP
 * RAM starting at __rom_in_ram_start__. This header defines the
 * region's sub-block offsets (cart image, command sentinel, dirty
 * frame counter, indexed shared variables, APP_FREE, framebuffer)
 * plus the cart_asM68kLong() helper for exact-value uint32_t RP→
 * m68k writes (see project_cartbus_long_byteswap memory note).
 *
 * The constants used to live in `chandler.h` under the `CHANDLER_*`
 * prefix. The chandler / TPROTOCOL command-channel machinery was
 * removed in Epic 3 Story 3.8; the layout constants survived the
 * cull because IKBD and the framebuffer pipeline both need them.
 *
 * Layout must match target/atarist/src/main.s on the m68k side.
 */

#ifndef CART_SHARED_H
#define CART_SHARED_H

#include <inttypes.h>
#include <stdbool.h>

/* All offsets are relative to __rom_in_ram_start__, which mirrors
 * ROM4_ADDR ($FA0000) on the m68k side. Layout (single source of
 * truth, must match target/atarist/src/main.s):
 *
 *   $FA0000  CARTRIDGE             m68k header + code (max 16 KB).
 *                                  Includes the unrolled MOVEM-loop
 *                                  block (fbdrv) at offset $2000.
 *   $FA4000  CMD_MAGIC_SENTINEL    4 B  (m68k polls here for
 *                                        NOP / RESET / BOOT_GEM / START)
 *   $FA4004  (reserved)            8 B  (was RANDOM_TOKEN +
 *                                        RANDOM_TOKEN_SEED for the
 *                                        former TPROTOCOL handshake;
 *                                        unused since Epic 3 Story 3.8)
 *   $FA400C  FB_FRAME_COUNTER      4 B  (RP-incremented dirty-frame
 *                                        marker; m68k VBL loop skips
 *                                        the cart->ST blit when this
 *                                        is unchanged since the
 *                                        previous iteration)
 *   $FA4010  SHARED_VARIABLES    240 B  (60 indexed 4-byte slots,
 *                                        app-free).
 *   $FA4100  APP_FREE           ~16.5 KB free arena, ends at FRAMEBUFFER
 *   $FA8300  FRAMEBUFFER          32 KB (320x200 4 bpp low-res)
 *   $FAFFFF  end of region
 */
#define CART_CARTRIDGE_CODE_SIZE         0x4000  /* 16 KB cart-image budget */
#define CART_SHARED_BLOCK_OFFSET         CART_CARTRIDGE_CODE_SIZE
#define CART_CMD_SENTINEL_OFFSET         CART_SHARED_BLOCK_OFFSET
#define CART_FB_FRAME_COUNTER_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x0C)
#define CART_SHARED_VARIABLES_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x10)
#define CART_SHARED_VARIABLES_SLOTS      60      /* 240 bytes total */

/* APP_FREE arena starts directly after SHARED_VARIABLES; the audio
 * pipeline (Epic 4) was removed and its 1 KB was rolled back into
 * the free arena. */
#define CART_APP_FREE_OFFSET                                                  \
  (CART_SHARED_VARIABLES_OFFSET + (CART_SHARED_VARIABLES_SLOTS * 4))

/* Framebuffer sized for low-res 4 bpp (320 x 200 = 32000 bytes). Sits
 * flush against the top of the 64 KB region: end = $FB0000 exactly,
 * start = $FB0000 - 32000 = $FA8300. APP_FREE's upper bound is
 * implicitly the framebuffer base. */
#define CART_FRAMEBUFFER_SIZE         32000
#define CART_FRAMEBUFFER_OFFSET       (0x10000 - CART_FRAMEBUFFER_SIZE)
#define CART_REGION_END               0x10000  /* 64 KB shared region top */

/* RP→m68k command sentinel values. The m68k polls the longword at
 * CART_CMD_SENTINEL_OFFSET; non-zero values steer it out of the
 * userfw loop or the bootstrap dispatcher. Must match the m68k-side
 * equs in target/atarist/src/main.s. */
#define CART_CMD_NOP        0u
#define CART_CMD_RESET      1u
#define CART_CMD_BOOT_GEM   2u
#define CART_CMD_START      4u

/* The cart bus byte-swaps WITHIN each 16-bit word: RP stores LE,
 * m68k reads BE, and the swap makes that transparent for uint16_t.
 * For uint32_t, m68k's BE long-read is two word reads in (high, low)
 * order, but the two 16-bit halves stay in their RP-LE positions --
 * so m68k sees the halves SWAPPED.
 *
 * For exact-value RP→m68k longword protocols (CMD_MAGIC_SENTINEL,
 * etc.) the RP must store the half-swapped value so the m68k's
 * move.l observes the intended uint32_t. Protocols that only care
 * about inequality (the FB dirty-frame counter is the canonical
 * example) don't need this -- both sides see distinct values for
 * distinct writes regardless of the swap. */
static inline uint32_t cart_asM68kLong(uint32_t v) {
  return (v << 16) | (v >> 16);
}

#endif /* CART_SHARED_H */
