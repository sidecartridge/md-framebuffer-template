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
 * removed; the layout constants survived the
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
 *                                        unused)
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

/* 16-entry ST palette published by the RP, applied by the m68k VBL
 * handler to $FFFF8240..$FFFF825E each frame. Format: 16 contiguous
 * 16-bit words. Each word is the standard ST 9-bit palette format
 * 0000.0RRR.0GGG.0BBB. uint16_t writes are transparent across the
 * cart-bus byte-swap so the RP can write the m68k-observable word
 * value directly.
 *
 * Lives inside SHARED_VARIABLES (slots 12..19 = offsets +0x30..0x4F
 * = absolute $FA4040..$FA405F). Apps that don't want RP-driven
 * palette publishing can leave the slot at zeros (= black palette
 * = all-black screen) and write $FFFF8240 from their own m68k code
 * instead. */
#define CART_PALETTE_OFFSET                                                   \
  (CART_SHARED_VARIABLES_OFFSET + (12 * 4))      /* $4040 */
#define CART_PALETTE_ENTRIES             16
#define CART_PALETTE_SIZE                (CART_PALETTE_ENTRIES * 2)  /* 32 B */

/* Audio sample buffer. Single-channel YM2149 ch A 4-bit DAC: each
 * byte holds a YM volume nibble (0..15) in its low 4 bits. The m68k
 * Timer-B IRQ handler fires at ~6.27 kHz and reads one byte per
 * fire, wrapping the read pointer at CART_AUDIO_BUFFER_SIZE. The
 * RP-side audio.c fills the buffer with samples mapped through a
 * logarithmic LUT (linear PCM -> closest matching YM volume). */
#define CART_AUDIO_BUFFER_OFFSET                                              \
  (CART_SHARED_VARIABLES_OFFSET + (CART_SHARED_VARIABLES_SLOTS * 4))
#define CART_AUDIO_BUFFER_SIZE           1024

/* APP_FREE arena starts after the audio buffer. */
#define CART_APP_FREE_OFFSET                                                  \
  (CART_AUDIO_BUFFER_OFFSET + CART_AUDIO_BUFFER_SIZE)

/* Framebuffer sized for low-res 4 bpp (320 x 200 = 32000 bytes). Sits
 * flush against the top of the 64 KB region: end = $FB0000 exactly,
 * start = $FB0000 - 32000 = $FA8300. APP_FREE's upper bound is
 * implicitly the framebuffer base. */
#define CART_FRAMEBUFFER_SIZE         32000
#define CART_FRAMEBUFFER_OFFSET       (0x10000 - CART_FRAMEBUFFER_SIZE)
#define CART_REGION_END               0x10000  /* 64 KB shared region top */

/* ---------------------------------------------------------------------
 * Framebuffer chunk layout for the m68k MOVEM blit
 *
 * The m68k FBDRV_INLINE macro copies the cart framebuffer into a
 * hidden ST screen page using a fully unrolled
 *
 *     movem.l (a6)+, d0-d7/a1-a4        ; read 48 bytes
 *     movem.l d0-d7/a1-a4, -(a5)        ; predec store, reverse-order
 *
 * pair per iteration (A0 and A7 omitted from the list: A0 is the
 * dedicated Timer-B audio buffer pointer, A7 keeps the supervisor
 * SP valid so IRQs may fire during the macro). The predec store
 * mode is 4 cycles per iter faster than `d16(a5)` displacement mode
 * (8+8n vs 12+8n on 68000), but it writes each 12-longword group in
 * REVERSE memory order relative to the source -- chunks land in the
 * destination screen page from the END (page+31968) down to the
 * START (page+0).
 *
 * For the screen to display the correct image, the cart-FB at
 * $FA8300 must therefore be laid out with the image's 48-byte chunks
 * already pre-reversed:
 *
 *     cart-FB chunk K (bytes K*48 .. K*48+47)
 *       holds image chunk (665-K)
 *
 *   i.e.:
 *     cart-FB bytes      0 ..    47   <-- image bytes 31920 .. 31967
 *     cart-FB bytes     48 ..    95   <-- image bytes 31872 .. 31919
 *     ...
 *     cart-FB bytes  31920 .. 31967   <-- image bytes     0 ..    47
 *     cart-FB bytes  31968 .. 31999   <-- natural-order 32-byte tail
 *
 * Within each 48-byte chunk the bytes are in natural order; only the
 * chunk-level sequence is reversed.
 *
 * The 32-byte tail at image bytes 31968..31999 (= last 64 pixels of
 * scanline 199) is handled by a separate small `d16(a5)` MOVEM
 * after the main predec unroll, so the RP leaves those bytes in
 * NATURAL (non-reversed) order at cart-FB[31968..31999].
 *
 * Chunks DO NOT align with scanlines (LCM(48, 160) = 480), so this
 * is not a simple row reversal -- each m68k chunk covers exactly
 * scanline (112 pixels at 4 bpp) and may span row boundaries. The
 * RP-side c2p (rp/src/fb_chunked_asm.S + fb_chunked.c) is responsible
 * for emitting the reversed layout. The simplest implementation is
 * to keep c2p's natural row-major output going to a 32 KB scratch
 * buffer in RP RAM, then do a chunk-reversed memcpy from scratch to
 * the cart FB once both cores finish (~120 us / frame, well under
 * fb_render_frame's main-loop budget). Per-byte address arithmetic
 * inside the c2p hot path is also possible but more invasive.
 *
 * Cost / benefit:
 *   - m68k saves ~4 cyc/iter * 571 iters = ~2284 cyc / VBL (~285 us)
 *   - m68k boot adds one `lea (FB_CHUNK_COVERED)(a5), a5` per VBL (~12 cyc)
 *   - RP adds ~120 us / frame for the scratch->cart-FB reverse memcpy
 *   - Net: ~285 us VBL slack reclaimed on the m68k side
 */
#define CART_FB_CHUNK_BYTES           48   /* size of one m68k MOVEM-burst group (12 longwords; A0 and A7 omitted -- A0 is the dedicated Timer-B audio pointer, A7 is the SP) */
#define CART_FB_BLIT_LINES            200  /* must match FB_COPY_LINES in target/atarist/src/userfw.s */
#define CART_FB_BLIT_BYTES            (CART_FB_BLIT_LINES * 160)  /* total bytes the m68k blits per VBL (160 = ST 4bpp scanline) */
#define CART_FB_CHUNK_COUNT           (CART_FB_BLIT_BYTES / CART_FB_CHUNK_BYTES)  /* iterations of the unrolled MOVEM-pair */
#define CART_FB_CHUNK_COVERED         (CART_FB_CHUNK_BYTES * CART_FB_CHUNK_COUNT)
#define CART_FB_CHUNK_TAIL            (CART_FB_BLIT_BYTES - CART_FB_CHUNK_COVERED)  /* bytes copied by the m68k via d16(a5) MOVEM after the main predec unroll */

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
