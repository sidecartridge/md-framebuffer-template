/**
 * File: fb_chunked.c
 * Description: Chunked framebuffer storage + Core 0 / Core 1 dispatch
 *              for the chunky-to-planar conversion (Epic 2 Story 2.4).
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
 * Expected wallclock: ~1.0-1.3 ms per frame (each core ~1 ms of
 * compute + SRAM bank contention as both cores read chunked and
 * write planar simultaneously).
 */

#include "fb_chunked.h"

#include <string.h>

#include "pico/multicore.h"

uint8_t fb_chunked_buffer[FB_CHUNKED_SIZE] __attribute__((aligned(4)));

/* Asm worker (fb_chunked_asm.S). Processes pixels in [src, src_end)
 * into the planar layout starting at dst. */
extern void fb_c2p_half(uint16_t *dst,
                        const uint8_t *src,
                        const uint8_t *src_end);

#define FB_C2P_HALF_SRC_BYTES (FB_CHUNKED_SIZE / 2)       /* 32000 */
#define FB_C2P_HALF_DST_WORDS (FB_C2P_HALF_SRC_BYTES / 4) /* 8000 uint16_t */

/* Core 1 forever-loop. Pops a dst pointer for the bottom half, runs
 * the asm worker, signals completion. Placed in RAM so the inner
 * loop doesn't pay XIP cost on every dispatch. */
static void __not_in_flash_func(fb_c2p_core1_loop)(void) {
  for (;;) {
    uintptr_t dst_bottom = (uintptr_t)multicore_fifo_pop_blocking();
    fb_c2p_half((uint16_t *)dst_bottom,
                fb_chunked_buffer + FB_C2P_HALF_SRC_BYTES,
                fb_chunked_buffer + FB_CHUNKED_SIZE);
    multicore_fifo_push_blocking(0);
  }
}

void fb_chunked_init(void) {
  /* multicore_launch_core1 blocks until Core 1 has entered the user
   * function, so by the time fb_init returns, Core 1 is parked in the
   * FIFO pop and ready to service the first frame. */
  multicore_launch_core1(fb_c2p_core1_loop);
}

void fb_chunked_clear(uint8_t color) {
  memset(fb_chunked_buffer, color, sizeof(fb_chunked_buffer));
}

void __not_in_flash_func(fb_chunky_to_planar)(uint16_t *planar) {
  /* Dispatch bottom half to Core 1 (asynchronous, just queues). */
  multicore_fifo_push_blocking((uint32_t)(uintptr_t)(planar + FB_C2P_HALF_DST_WORDS));

  /* Top half: Core 0 runs the worker directly, in parallel with Core 1. */
  fb_c2p_half(planar,
              fb_chunked_buffer,
              fb_chunked_buffer + FB_C2P_HALF_SRC_BYTES);

  /* Wait for Core 1 to finish its half. */
  (void)multicore_fifo_pop_blocking();
}
