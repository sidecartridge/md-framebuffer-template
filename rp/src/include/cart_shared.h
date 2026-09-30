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
 * Layout must match target/atarist/src/inc/sidecart_layout.s on the m68k
 * side; tests/host/test_layout.py checks that they agree.
 */

#ifndef CART_SHARED_H
#define CART_SHARED_H

#include <inttypes.h>
#include <stdbool.h>

/* All offsets are relative to __rom_in_ram_start__, which mirrors
 * ROM4_ADDR ($FA0000) on the m68k side. Layout (single source of
 * truth, must match target/atarist/src/inc/sidecart_layout.s):
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
 *                                        app-free, but for slots 2..5:
 *                                        commands for the IKBD, slot 6:
 *                                        the audio output, 7: the copy
 *                                        mode, 8: the profile, and
 *                                        slots 12..19: the palette).
 *   $FA4100  AUDIO_BUFFER       4096 B (YM volume pairs, or DMA samples)
 *   $FA5100  BOOT_STATUS        2 B  (read once by pre_auto: 0 = start)
 *   $FA5102  BOOT_MESSAGE     126 B  (why the RP refused to start)
 *   $FA5180  APP_FREE          ~12.4 KB free arena, ends at FRAMEBUFFER
 *   $FA8300  FRAMEBUFFER          32 KB (320x200 4 bpp low-res)
 *   $FAFFFF  end of region
 */
#define CART_CARTRIDGE_CODE_SIZE         0x4000  /* 16 KB cart-image budget */
#define CART_SHARED_BLOCK_OFFSET         CART_CARTRIDGE_CODE_SIZE
#define CART_CMD_SENTINEL_OFFSET         CART_SHARED_BLOCK_OFFSET
#define CART_FB_FRAME_COUNTER_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x0C)
#define CART_SHARED_VARIABLES_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x10)
#define CART_SHARED_VARIABLES_SLOTS      60      /* 240 bytes total */

/* Commands for the IKBD, from the RP (ikbd.c): slots 2..5 of
 * SHARED_VARIABLES. A generation word, a length word and up to
 * CART_IKBD_OUT_MAX command bytes, two to a word, the first in the word's
 * high byte (what the m68k reads first). The ST reads the generation every
 * VBL; when it changes, the ST sends the bytes to the IKBD, one per VBL (a 0
 * sends nothing that VBL), then reports the generation's low byte through
 * CART_ROM3_IKBD_OUT_WINDOW. The RP sets CART_IKBD_OUT_BUSY in the
 * generation while it rewrites the block, and clears it with the new
 * generation last. */
#define CART_IKBD_OUT_OFFSET                                                  \
  (CART_SHARED_VARIABLES_OFFSET + (2 * 4))       /* $4018 */
#define CART_IKBD_OUT_SIZE               16
#define CART_IKBD_OUT_MAX                (CART_IKBD_OUT_SIZE - 4)
#define CART_IKBD_OUT_BUSY               0x8000u

/* Which way the ST plays the sound, as the RP asks: a word in slot 6 of
 * SHARED_VARIABLES, read once per ST boot. CART_AUDIO_OUT_AUTO (0, what
 * the window's erase leaves) takes the DMA sound chip on an STE or a Mega
 * STE and the YM elsewhere; CART_AUDIO_OUT_YM keeps to the YM (audio.c). */
#define CART_AUDIO_OUT_OFFSET                                                 \
  (CART_SHARED_VARIABLES_OFFSET + (6 * 4))       /* $4028 */
#define CART_AUDIO_OUT_AUTO              0u
#define CART_AUDIO_OUT_YM                1u

/* Who copies the cart framebuffer to the ST's screen page, as the RP asks
 * (fb_set_copy_mode()): a word in slot 7 of SHARED_VARIABLES, read every
 * VBL. The low byte is the mode, the high byte the chunks of 48 bytes the
 * blitter copies at a time (0: the ST's default). CART_BLIT_MODE_AUTO (0,
 * what the window's erase leaves) takes the blitter on the DMA sound path
 * and the CPU elsewhere. Without a blitter the CPU copies. */
#define CART_BLIT_MODE_OFFSET                                                 \
  (CART_SHARED_VARIABLES_OFFSET + (7 * 4))       /* $402C */
#define CART_BLIT_MODE_AUTO              0u
#define CART_BLIT_MODE_CPU               1u
#define CART_BLIT_MODE_BLITTER           2u

/* The app's profile (profile.h), chosen at compile time: how often the ST
 * takes a new frame and how fast it plays the sound. The RP writes it at
 * CART_PROFILE_OFFSET, slot 8 of SHARED_VARIABLES, before the ST boots;
 * userfw reads it once. Per profile: the VBLs a frame; on the YM path
 * Timer-B's count (/4: 614,400 Hz / count), the rate it plays and the bytes
 * of a VBL (two a sample); on the DMA path the chip's mode, its rate, the
 * bytes of a VBL (rounded up) and the lead: the bytes the ST keeps copied
 * ahead of the chip, 3 VBLs, so a missed VBL still finds samples. */
#define CART_PROFILE_OFFSET                                                   \
  (CART_SHARED_VARIABLES_OFFSET + (8 * 4))       /* $4030 */
#define CART_PROFILE_50FPS               0u
#define CART_PROFILE_25FPS               1u
#define CART_PROFILE_50FPS_VBLS          1
#define CART_PROFILE_50FPS_TIMERB_COUNT  110
#define CART_PROFILE_50FPS_YM_RATE_HZ    5585u
#define CART_PROFILE_50FPS_YM_BYTES      224u
#define CART_PROFILE_50FPS_DMA_MODE      0x81   /* mono, 12,517 Hz */
#define CART_PROFILE_50FPS_DMA_RATE_HZ   12517u
#define CART_PROFILE_50FPS_DMA_BYTES     256u
#define CART_PROFILE_50FPS_DMA_LEAD      768
#define CART_PROFILE_25FPS_VBLS          2
#define CART_PROFILE_25FPS_TIMERB_COUNT  28
#define CART_PROFILE_25FPS_YM_RATE_HZ    21943u
#define CART_PROFILE_25FPS_YM_BYTES      878u
#define CART_PROFILE_25FPS_DMA_MODE      0x82   /* mono, 25,033 Hz */
#define CART_PROFILE_25FPS_DMA_RATE_HZ   25033u
#define CART_PROFILE_25FPS_DMA_BYTES     512u
#define CART_PROFILE_25FPS_DMA_LEAD      1536

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

/* Audio sample buffer, used one of two ways (audio.c):
 *   - the YM: (vA, vB) YM2149 volume pairs, two bytes per sample for
 *     channels A and B. The m68k Timer-B IRQ handler fires at the profile's
 *     rate and reads one pair per fire; its VBL handler points the read
 *     cursor at the start of the next slice every VBL (below), so a frame
 *     reads the first 224 (or 878) bytes of one slice;
 *   - the DMA sound chip of an STE or a Mega STE: the whole buffer mirrors
 *     the ring of 8-bit signed samples the chip plays from ST RAM at the
 *     profile's rate. Every VBL the ST reports where the chip plays
 *     (CART_ROM3_DMA_POS_WINDOW, by CART_AUDIO_DMA_POS_UNIT bytes) and
 *     copies the mirror up to the profile's lead ahead of it; the RP writes
 *     further ahead. Byte i of the ring is byte i ^ 1 of the RP's buffer
 *     (the cart bus swaps the bytes of each word). */
#define CART_AUDIO_BUFFER_OFFSET                                              \
  (CART_SHARED_VARIABLES_OFFSET + (CART_SHARED_VARIABLES_SLOTS * 4))
#define CART_AUDIO_BUFFER_SIZE           4096
#define CART_AUDIO_DMA_RING_BYTES        CART_AUDIO_BUFFER_SIZE
#define CART_AUDIO_DMA_POS_UNIT          16
/* The buffer is CART_AUDIO_SLICES slices of one VBL each. The ST's VBL
 * handler moves Timer-B to the next slice and reports which one through
 * CART_ROM3_AUDIO_SLICE_WINDOW; the RP writes only the slices after it
 * (audio.c). A VBL plays 224 bytes of a slice's 1024 at 5,585 Hz, 878 at
 * 21,943 Hz. */
#define CART_AUDIO_SLICES                4
#define CART_AUDIO_SLICE_BYTES           1024

/* Boot block, after the audio buffer. The ST reads the status word once
 * per boot, in pre_auto, before it starts userfw: CART_BOOT_OK (0, which
 * the window's erase at boot leaves) starts the app; anything else makes
 * pre_auto print the NUL-terminated text at CART_BOOT_MESSAGE_OFFSET and
 * return to GEM. Written through st_session_veto_boot(). */
#define CART_BOOT_STATUS_OFFSET                                               \
  (CART_AUDIO_BUFFER_OFFSET + CART_AUDIO_BUFFER_SIZE)
#define CART_BOOT_OK                  0u
#define CART_BOOT_VETOED              1u
#define CART_BOOT_MESSAGE_OFFSET      (CART_BOOT_STATUS_OFFSET + 2)
#define CART_BOOT_MESSAGE_SIZE        126  /* bytes, the NUL included */

/* APP_FREE arena starts after the boot block. */
#define CART_APP_FREE_OFFSET                                                  \
  (CART_BOOT_MESSAGE_OFFSET + CART_BOOT_MESSAGE_SIZE)

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
#define CART_FB_BLIT_LINES            200  /* must match FB_COPY_LINES in target/atarist/src/inc/sidecart_layout.s */
#define CART_FB_BLIT_BYTES            (CART_FB_BLIT_LINES * 160)  /* total bytes the m68k blits per VBL (160 = ST 4bpp scanline) */
#define CART_FB_CHUNK_COUNT           (CART_FB_BLIT_BYTES / CART_FB_CHUNK_BYTES)  /* iterations of the unrolled MOVEM-pair */
#define CART_FB_CHUNK_COVERED         (CART_FB_CHUNK_BYTES * CART_FB_CHUNK_COUNT)
#define CART_FB_CHUNK_TAIL            (CART_FB_BLIT_BYTES - CART_FB_CHUNK_COVERED)  /* bytes copied by the m68k via d16(a5) MOVEM after the main predec unroll */

/* RP→m68k command sentinel values. The m68k polls the longword at
 * CART_CMD_SENTINEL_OFFSET; non-zero values steer it out of the
 * userfw loop or the bootstrap dispatcher. Must match the m68k-side
 * equs in target/atarist/src/inc/sidecart_layout.s. */
#define CART_CMD_NOP        0u
#define CART_CMD_RESET      1u
#define CART_CMD_BOOT_GEM   2u
#define CART_CMD_START      4u

/* ROM3 signalling windows. The ST cannot write the cartridge window, so it
 * tells the RP things by reading ROM3 addresses ($FB0000-$FBFFFF), which
 * commemul's ring captures: the high byte of the captured address says what
 * the read means, the low byte carries a value. Must match the equs in
 * target/atarist/src/inc/sidecart_layout.s.
 *
 *   $FB82xx  an IKBD byte (ikbd.c, IKBD_WINDOW_LO16)
 *   $FB83xx  VBL: the ST has read xx IKBD bytes so far, mod 256 (ikbd.c)
 *   $FB84xx  blit done: the cart framebuffer is free (fb.c)
 *   $FB8500  the keyboard ACIA overran: IKBD bytes were lost (ikbd.c)
 *   $FB86xx  VBL: Timer-B plays audio slice xx from now on (audio.c)
 *   $FB87xx  the ST sent the IKBD commands of generation xx (low byte)
 *   $FB88xx  hello: a new ST session starts; xx is the machine (st_session.h)
 *   $FB89xx  TOS version, high byte; sent just before the hello
 *   $FB8Axx  TOS version, low byte; sent just before the hello
 *   $FB8Bxx  the stopwatch (userfw.s TIME_STUDY): point xx of the ST's loop,
 *   $FB8Cxx  then its ticks, high byte, and
 *   $FB8Fxx  low byte (fb.c)
 *   $FB8Dxx  VBL: the DMA sound chip plays byte xx * CART_AUDIO_DMA_POS_UNIT
 *            of its ring (audio.c)
 *   $FB8Exx  the ST's features (CART_ST_FEATURE_*), after the hello */
#define CART_ROM3_WINDOW_MASK        0xFF00u
#define CART_ROM3_IKBD_COUNT_WINDOW  0x8300u
#define CART_ROM3_BLIT_DONE_WINDOW   0x8400u
#define CART_ROM3_IKBD_OVERRUN_WINDOW 0x8500u
#define CART_ROM3_AUDIO_SLICE_WINDOW 0x8600u
#define CART_ROM3_IKBD_OUT_WINDOW    0x8700u
#define CART_ROM3_HELLO_WINDOW       0x8800u
#define CART_ROM3_TOS_HI_WINDOW      0x8900u
#define CART_ROM3_TOS_LO_WINDOW      0x8A00u
#define CART_ROM3_STUDY_POINT_WINDOW 0x8B00u
#define CART_ROM3_STUDY_HI_WINDOW    0x8C00u
#define CART_ROM3_STUDY_LO_WINDOW    0x8F00u
#define CART_ROM3_DMA_POS_WINDOW     0x8D00u
#define CART_ROM3_ST_FEATURES_WINDOW 0x8E00u
#define CART_ST_FEATURE_BLITTER      0x01u

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
