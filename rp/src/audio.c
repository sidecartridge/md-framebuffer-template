/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The m68k Timer-B IRQ reads sample bytes from the cart buffer at
 * CART_AUDIO_BUFFER_OFFSET (1024 B). The RP refills the buffer
 * once per VBL via audio_render_frame() (paced to ~50 Hz via
 * time_us_32). The library is format-agnostic -- it just dispatches
 * to whatever fill callback the app has installed.
 *
 * See audio.h for the public API and `audio_play_loop` /
 * `audio_set_fill_callback` semantics.
 */

#include "audio.h"

#include <stdint.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Per-VBL fill cadence. Refill every ~20 ms (one PAL VBL) so the
 * RP keeps pace with the m68k's Timer-B reads without burning more
 * main-loop cycles than necessary. */
#define AUDIO_FRAME_PERIOD_US 20000u

/* m68k Timer-B consumption per PAL VBL. Must stay in sync with the
 * Timer-B rate in target/atarist/src/userfw.s:
 *   TBDR=110 /4 prescaler -> 5,585.45 Hz -> 111.71 samples/VBL
 *   = 223.43 B/VBL @ 2 B/sample (dual-channel mode).
 * Rounded up to 224. The remaining ~800 B of CART_AUDIO_BUFFER_SIZE
 * is intentional headroom -- not consumed within a single VBL, but
 * left as a safety pad if the m68k's A0 cursor ever overruns its
 * per-VBL budget. */
#define AUDIO_FILL_BYTES_PER_VBL 224u

static uint8_t *s_audio_buf;
static uint32_t s_last_frame_us;
static audio_fill_cb_t s_fill_cb;

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_last_frame_us = 0;
  s_fill_cb = NULL;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence on YM). With no callback installed, the
   * buffer stays zero until an app calls audio_play_loop() or
   * audio_set_fill_callback(). */

  DPRINTF("audio_init: cart buffer %u B at offset $%04X, %u B/VBL refill\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE,
          (unsigned)CART_AUDIO_BUFFER_OFFSET,
          (unsigned)AUDIO_FILL_BYTES_PER_VBL);
}

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_fill_cb = cb;
}

static void audio_loop_cb(uint8_t *buf, uint32_t bytes) {
  uint32_t pos = s_loop_pos;
  const uint32_t total = s_loop_bytes;
  const uint8_t *src = s_loop_data;
  for (uint32_t i = 0; i < bytes; i++) {
    buf[i] = src[pos];
    pos++;
    if (pos >= total) {
      pos = 0;  /* loop */
    }
  }
  s_loop_pos = pos;
}

void audio_play_loop(const uint8_t *data, uint32_t bytes) {
  s_loop_data = data;
  s_loop_bytes = bytes;
  s_loop_pos = 0;
  s_fill_cb = audio_loop_cb;
}

void audio_render_frame(void) {
  if (s_fill_cb == NULL) {
    return;
  }

  uint32_t now_us = time_us_32();
  if (s_last_frame_us != 0 &&
      (now_us - s_last_frame_us) < AUDIO_FRAME_PERIOD_US) {
    return;
  }
  s_last_frame_us = now_us;

  s_fill_cb(s_audio_buf, AUDIO_FILL_BYTES_PER_VBL);
}
