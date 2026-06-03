/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer (single-channel
 *              YM2149 ch A 4-bit DAC).
 *
 * The m68k Timer-B IRQ at ~6.27 kHz reads one byte per fire from
 * the 256-byte cart buffer at CART_AUDIO_BUFFER_OFFSET. Each byte
 * is a YM volume nibble (0..15) in its low 4 bits.
 *
 * This file produces those bytes via:
 *   1. A logarithmic LUT (256 entries) mapping linear 8-bit PCM
 *      values to the closest-matching YM volume nibble. The YM2149
 *      volume curve is exponential: amp(v) = 2^((v-15)/2) for
 *      v >= 1, amp(0) = 0. We pre-search the closest match for each
 *      linear target once at boot.
 *   2. A simple sine generator (~440 Hz at 6269 Hz sample rate) as
 *      the test signal source. Replace with a real PCM stream when
 *      ready -- the LUT layer is independent of the source.
 *
 * audio_render_frame() is called from emul.c's main loop; it paces
 * the cart-buffer fills via time_us_32 to roughly match the m68k's
 * consumption rate.
 */

#include "audio.h"

#include <math.h>
#include <stdint.h>

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* Sample rate (m68k Timer-B at /4 prescaler, TBDR=98 on a 2.4576 MHz
 * MFP clock). Match TIMERB_PRESCALER / TIMERB_COUNT in userfw.s. */
#define AUDIO_SAMPLE_RATE_HZ 6269u

/* PCM-to-YM-volume LUT. Index = linear PCM target (0..255, with 0
 * = silence and 255 = full amplitude), value = closest YM volume
 * nibble (0..15) by acoustic amplitude. */
static uint8_t s_pcm_to_ym_lut[256];

static uint8_t *s_audio_buf;

/* Sine phase (radians) accumulator + delta. Float for simplicity; a
 * fixed-point version would be cheaper but it doesn't matter -- the
 * RP has cycles to burn at 260 MHz vs the m68k's 6.27 kHz consume. */
static float s_sine_phase = 0.0f;
#define AUDIO_TEST_TONE_HZ 440.0f

/* Per-frame fill cadence. Refill every ~20 ms (one PAL VBL) so the
 * RP keeps pace with the m68k's Timer-B reads without burning more
 * main-loop cycles than necessary. */
#define AUDIO_FRAME_PERIOD_US 20000u
static uint32_t s_last_frame_us = 0;
static uint32_t s_buf_write_pos = 0;

/* Build the PCM-to-YM volume LUT once at boot. The YM amplitude
 * curve is logarithmic; we pick the nibble whose absolute amplitude
 * is nearest to the linear target. The result is roughly the same
 * shape as Atari demos that build a 256-entry table by hand. */
static void build_pcm_to_ym_lut(void) {
  float ym_amp[16];
  ym_amp[0] = 0.0f;
  for (int v = 1; v < 16; v++) {
    ym_amp[v] = powf(2.0f, ((float)v - 15.0f) / 2.0f);
  }
  for (int p = 0; p < 256; p++) {
    float target = (float)p / 255.0f;  /* normalize to [0, 1] */
    int best_v = 0;
    float best_diff = fabsf(ym_amp[0] - target);
    for (int v = 1; v < 16; v++) {
      float d = fabsf(ym_amp[v] - target);
      if (d < best_diff) {
        best_diff = d;
        best_v = v;
      }
    }
    s_pcm_to_ym_lut[p] = (uint8_t)best_v;
  }
}

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;

  build_pcm_to_ym_lut();
  s_sine_phase = 0.0f;
  s_last_frame_us = 0;
  s_buf_write_pos = 0;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the buffer
   * (= silence on YM). */

  DPRINTF(
      "audio_init: single-ch A YM DAC, LUT built, %u bytes buffer, "
      "%u Hz Timer-B rate\n",
      (unsigned)CART_AUDIO_BUFFER_SIZE,
      (unsigned)AUDIO_SAMPLE_RATE_HZ);
}

void audio_render_frame(void) {
  /* Pace to ~50 Hz. Each call refills CART_AUDIO_BUFFER_SIZE / 2
   * samples (128) -- a little more than one VBL's worth of Timer-B
   * reads (~125 samples per VBL at 6269 Hz), so the m68k always
   * sees fresh-enough data. The write pointer cycles through the
   * 256-byte buffer at the same rate the m68k's read pointer does. */
  uint32_t now_us = time_us_32();
  if (s_last_frame_us != 0 &&
      (now_us - s_last_frame_us) < AUDIO_FRAME_PERIOD_US) {
    return;
  }
  s_last_frame_us = now_us;

  const float dphase =
      2.0f * 3.14159265f * AUDIO_TEST_TONE_HZ / (float)AUDIO_SAMPLE_RATE_HZ;
  const uint32_t samples_this_frame = CART_AUDIO_BUFFER_SIZE / 2u;

  uint32_t pos = s_buf_write_pos;
  for (uint32_t i = 0; i < samples_this_frame; i++) {
    float s = sinf(s_sine_phase);
    s_sine_phase += dphase;
    if (s_sine_phase > 6.28318530f) {
      s_sine_phase -= 6.28318530f;
    }

    /* Map [-1, +1] to PCM [0, 255], then LUT to YM nibble. */
    int p = (int)((s + 1.0f) * 127.5f);
    if (p < 0) {
      p = 0;
    } else if (p > 255) {
      p = 255;
    }
    s_audio_buf[pos] = s_pcm_to_ym_lut[p];

    pos = (pos + 1u) & (CART_AUDIO_BUFFER_SIZE - 1u);
  }
  s_buf_write_pos = pos;
}
