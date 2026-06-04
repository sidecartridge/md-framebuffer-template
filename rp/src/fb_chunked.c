/**
 * File: fb_chunked.c
 * Description: Chunked framebuffer storage + Core 0 / Core 1 dispatch
 *              for the chunky-to-planar conversion.
 *
 * `fb_c2p_half(dst, src, src_end)` lives in fb_chunked_asm.S -- the
 * multiplication-based bit-transpose worker that converts a range of
 * the chunked buffer into a contiguous slice of the planar destination.
 *
 * fb_chunky_to_planar() splits the work across both RP2040 cores:
 * Core 0 handles the top 100 rows (pixels 0..15999 of the chunked
 * buffer -> first 8000 uint16_t of planar), Core 1 handles the bottom
 * 100 rows. Sync is a one-word push/pop on the inter-core FIFO.
 *
 * Core 1 runs a perpetual loop in `fb_c2p_core1_loop` waiting for
 * each frame's dispatch. `fb_chunked_init()` launches it once at
 * boot via the pico-sdk's `multicore_launch_core1`.
 *
 * Output staging: both cores write their natural row-major planar
 * output into `fb_planar_scratch` in RP RAM. Once both halves are
 * complete, fb_chunky_to_planar copies the planar bytes into the
 * caller's `planar` (the cart framebuffer at $FA8300) with the
 * 48-byte m68k MOVEM chunks pre-reversed -- the m68k FBDRV_INLINE
 * macro uses `movem.l list, -(a5)` predec stores, which land each
 * chunk in the destination ST page in reverse memory order. See the
 * "Framebuffer chunk layout for the m68k MOVEM blit" comment in
 * cart_shared.h for the full spec.
 *
 * Expected wallclock: ~1.0-1.3 ms per frame for the dual-core c2p
 * + the chunk-reversed copy (666 * 48-byte chunks via the ldmia/stmia
 * asm worker fb_chunk_reverse_copy48).
 */

#include "fb_chunked.h"

#include <string.h>

#include "cart_shared.h"
#include "pico/multicore.h"

/* Per-file -O3: the global build is MinSizeRel (-Os). The chunky->planar
 * conversion + chunk-reversed memcpy here run on the hot per-frame path
 * and are pure compute; the dual-core handshake uses blocking FIFO calls
 * (real SDK calls with barriers), so -O3 is safe. */
#pragma GCC optimize("O3")

uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE] __attribute__((aligned(4)));

/* Asm worker (fb_chunked_asm.S). Processes pixels in [src, src_end)
 * into the planar layout starting at dst. */
extern void fb_c2p_half(uint16_t *dst,
                        const uint8_t *src,
                        const uint8_t *src_end);

#define FB_C2P_HALF_SRC_BYTES (FB_CHUNKED_SIZE / 2)       /* 32000 */
#define FB_C2P_HALF_DST_WORDS (FB_C2P_HALF_SRC_BYTES / 4) /* 8000 uint16_t */

/* Planar scratch buffer: c2p outputs natural row-major planar bytes
 * here; fb_chunky_to_planar then copies them to the caller's `planar`
 * (the cart FB at $FA8300) with the m68k MOVEM chunks pre-reversed.
 * 32 KB on the RP's 128 KB SRAM, 4-byte aligned for uint32 stores. */
static uint16_t fb_planar_scratch[CART_FRAMEBUFFER_SIZE / sizeof(uint16_t)]
    __attribute__((aligned(4)));

/* Generic Core 1 worker loop (dual-core). Pops a job function
 * pointer + arg off the FIFO, runs it, signals completion. Both the c2p
 * bottom half and the demos' band rendering dispatch through this. The
 * fn/arg travel through the FIFO (not shared memory), so the FIFO's
 * push/pop barriers fully order the handoff. Placed in RAM so the loop
 * doesn't pay XIP cost on every dispatch. */
static void __not_in_flash_func(fb_core1_loop)(void) {
  for (;;) {
    fb_core1_job_t job = (fb_core1_job_t)(uintptr_t)multicore_fifo_pop_blocking();
    void *arg = (void *)(uintptr_t)multicore_fifo_pop_blocking();
    job(arg);
    multicore_fifo_push_blocking(0); /* done */
  }
}

void __not_in_flash_func(fb_core1_dispatch)(fb_core1_job_t job, void *arg) {
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)job);
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)arg);
}

void __not_in_flash_func(fb_core1_wait)(void) {
  (void)multicore_fifo_pop_blocking(); /* wait for done */
}

/* c2p bottom-half job: arg is the bottom-half planar dst pointer. */
static void __not_in_flash_func(fb_c2p_bottom_job)(void *arg) {
  fb_c2p_half((uint16_t *)arg,
              fb_chunked_buffer + FB_C2P_HALF_SRC_BYTES,
              fb_chunked_buffer + FB_CHUNKED_SIZE);
}

void fb_chunked_init(void) {
  /* multicore_launch_core1 blocks until Core 1 has entered the user
   * function, so by the time fb_init returns, Core 1 is parked in the
   * FIFO pop and ready to service the first frame. */
  multicore_launch_core1(fb_core1_loop);
}

void fb_chunked_clear(uint8_t color) {
  memset(fb_chunked_buffer, color, sizeof(fb_chunked_buffer));
}

void __not_in_flash_func(fb_transpose)(void) {
  /* Dispatch bottom half to Core 1. Both cores write into the
   * scratch buffer (natural row-major layout). */
  fb_core1_dispatch(fb_c2p_bottom_job,
                    fb_planar_scratch + FB_C2P_HALF_DST_WORDS);

  /* Top half: Core 0 runs the worker directly, in parallel with Core 1. */
  fb_c2p_half(fb_planar_scratch,
              fb_chunked_buffer,
              fb_chunked_buffer + FB_C2P_HALF_SRC_BYTES);

  /* Wait for Core 1 to finish its half. The join is a synchronization
   * barrier: pico-sdk's FIFO primitives include DMB/DSB internally, so
   * Core 1's planar writes are guaranteed visible to Core 0 by the
   * time this returns. */
  fb_core1_wait();
}

/* Thumb asm (fb_chunked_asm.S): the chunk-reversed copy via ldmia/stmia.
 * Replaces a per-chunk memcpy() loop -- GCC at -O3 can't prove the
 * uint8_t* pointers are word-aligned and emitted a `bl memcpy` per chunk
 * (666/frame); the cart FB + scratch are in fact 4-byte aligned. */
extern void fb_chunk_reverse_copy48(uint8_t *dst, const uint8_t *src_last,
                                    uint32_t count);
_Static_assert(CART_FB_CHUNK_BYTES == 48,
               "fb_chunk_reverse_copy48 hard-codes 48-byte (12-word) chunks");

void __not_in_flash_func(fb_planar_publish)(uint16_t *planar) {
  /* Chunk-reversed publish from scratch -> cart FB. cart-FB chunk K
   * is filled with scratch chunk (last-1-k), so the m68k's predec
   * MOVEM blit lands each chunk at its natural image position. This is
   * the ONLY cart-FB write -- it must run in the m68k's post-blit slack
   * (gated by fb_publish's VBL wait), and it's fast so it fits. The slow
   * transpose above runs unsynchronized (RP scratch RAM) and overlaps
   * the m68k blit. */
  const uint8_t *scratch_bytes = (const uint8_t *)fb_planar_scratch;
  uint8_t *cart_bytes = (uint8_t *)planar;

  /* src_last points at the final scratch chunk; the asm walks it back one
   * chunk per iteration, copying each 48-byte chunk forward. */
  fb_chunk_reverse_copy48(cart_bytes,
                          scratch_bytes + (CART_FB_CHUNK_COUNT - 1u) *
                                              (uint32_t)CART_FB_CHUNK_BYTES,
                          CART_FB_CHUNK_COUNT);

  /* Tail: the final CART_FB_CHUNK_TAIL bytes (bottom-right pixels)
   * don't fit a MOVEM chunk; the m68k copies them with a d16(a5) MOVEM
   * in NATURAL order, so copy them straight from scratch. */
  memcpy(cart_bytes + CART_FB_CHUNK_COVERED,
         scratch_bytes + CART_FB_CHUNK_COVERED,
         CART_FB_CHUNK_TAIL);
}

void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar) {
  /* Convenience wrapper: transpose then publish in one call (no VBL
   * sync). fb.c's fb_publish() calls the two halves separately so the
   * transpose can overlap the m68k blit and only the publish waits. */
  fb_transpose();
  fb_planar_publish(planar);
}
