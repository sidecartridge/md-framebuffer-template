/**
 * File: audio.c
 * Description: Cart-shared audio buffer producer.
 *
 * The m68k Timer-B IRQ plays (vA, vB) pairs from the cart buffer at
 * CART_AUDIO_BUFFER_OFFSET, one slice of CART_AUDIO_SLICES per VBL: the
 * ST's VBL handler moves Timer-B to the next slice and reports which one
 * through the ROM3 window CART_ROM3_AUDIO_SLICE_WINDOW. The RP writes only
 * the slices ahead of the one playing, so the ST never plays a slice while
 * it is being written.
 *
 * Two stages, so that the sound does not depend on the app's frame loop:
 *   - audio_render_frame(), from the main loop, tops up a small RAM FIFO
 *     with the app's fill callback (a loop in flash, a .YMS file on the SD
 *     card, the app's own generator);
 *   - a repeating timer interrupt (audio_writer) follows the ST's reports
 *     and moves one VBL of samples from the FIFO into each slice ahead.
 * The main loop may go AUDIO_FIFO_SLICES VBLs between two top-ups without
 * the sound noticing (80 ms by default): each VBL takes one slice from the
 * FIFO, the slices written ahead included. A sample reaches the ST at most
 * AUDIO_FIFO_SLICES + AUDIO_SLICES_AHEAD VBLs after the callback produced
 * it. When the FIFO is empty the slice holds the last sample (an underrun,
 * counted), never stale data.
 *
 * See audio.h for the public API and `audio_play_loop` /
 * `audio_set_fill_callback` semantics.
 */

#include "audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "ff.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* A PAL VBL, nominally. The slices follow the ST's own VBLs; this is the
 * period the sample arithmetic below is for. */
#define AUDIO_FRAME_PERIOD_US 20000u

/* One VBL of samples: what Timer-B plays between two VBLs. Must stay in
 * sync with the Timer-B rate in target/atarist/src/userfw.s:
 *   TBDR=110 /4 prescaler -> 5,585.45 Hz -> 111.71 samples/VBL
 *   = 223.43 B/VBL @ 2 B/sample (dual-channel mode).
 * Rounded up to 224, of a slice's CART_AUDIO_SLICE_BYTES. */
#define AUDIO_FILL_BYTES_PER_VBL 224u

/* Slices written ahead of the one the ST plays. */
#define AUDIO_SLICES_AHEAD 2u

/* The FIFO between the app's fill callback and the slice writer, in VBLs of
 * samples: the longest the main loop may go between two top-ups. More
 * delays the sound as much: up to AUDIO_FIFO_SLICES + AUDIO_SLICES_AHEAD
 * VBLs from the callback to the ST. */
#ifndef AUDIO_FIFO_SLICES
#define AUDIO_FIFO_SLICES 4u
#endif

/* The slice writer runs this often; the ST's report is seen at most this
 * long after its VBL, a slice ahead of when it is needed. */
#define AUDIO_WRITER_PERIOD_US 4000
/* How far back in the ROM3 ring the writer looks for the latest report: a
 * frame adds a few samples (the report, the blit acknowledgement, keys). */
#define AUDIO_REPORT_LOOKBACK 64u
/* No new report for this long: the ST left userfw (GEM, a reset). The next
 * report starts the count again. */
#define AUDIO_REPORT_TIMEOUT_US 200000u

static uint8_t *s_audio_buf;
static audio_fill_cb_t s_fill_cb;

static uint8_t s_fifo[AUDIO_FIFO_SLICES][AUDIO_FILL_BYTES_PER_VBL];
static volatile uint32_t s_fifo_head; /* slices produced (main loop) */
static volatile uint32_t s_fifo_tail; /* slices consumed (writer) */

static bool s_st_known;      /* the ST's slice is known */
static uint32_t s_st_vbl;    /* VBLs counted from the reports; its low bits are the slice */
static uint32_t s_next_vbl;  /* the next VBL whose slice is not written yet */
static uint16_t s_last_report;
static uint32_t s_last_report_us;
static uint8_t s_hold[2];    /* the last sample written: what an underrun holds */
static repeating_timer_t s_writer_timer;

/* Readable over SWD by their symbols (tools/dev/swd.py counters). */
uint32_t audioSlicesWritten;
uint32_t audioUnderruns;  /* slices written as a held sample: the FIFO was empty */
uint32_t audioLateSlices; /* slices the ST started before the RP had written them */

static bool audio_writer(repeating_timer_t *timer);

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

/* .YMS-file streaming state. audio_play_yms_file() validates the
 * 16-byte header, stores the data offset, and installs audio_yms_cb
 * as the per-VBL fill callback. The file is kept open for the
 * lifetime of playback. */
#define AUDIO_YMS_HEADER_SIZE     16u
#define AUDIO_YMS_MODE_DUAL_GHOST 1u
/* Must match TIMERB_COUNT in target/atarist/src/userfw.s:
 *   2.4576 MHz / 4 / 110 = 5,585.45 Hz */
#define AUDIO_NATIVE_RATE_HZ      5585u

static FIL s_yms_file;
static bool s_yms_open;
/* A read failed and has not succeeded since: logged once per streak. */
static bool s_yms_failing;
static FSIZE_t s_yms_data_offset;

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_fill_cb = NULL;
  s_yms_open = false;

  /* ERASE_FIRMWARE_IN_RAM at emul_start already zeroed the cart
   * buffer (= silence on YM). With no callback installed, the
   * buffer stays zero until an app calls audio_play_loop() or
   * audio_set_fill_callback(). */

  add_repeating_timer_us(-AUDIO_WRITER_PERIOD_US, audio_writer, NULL,
                         &s_writer_timer);

  DPRINTF("audio_init: %u slices of %u B at offset $%04X, %u B/VBL, "
          "FIFO %u VBLs\n",
          (unsigned)CART_AUDIO_SLICES, (unsigned)CART_AUDIO_SLICE_BYTES,
          (unsigned)CART_AUDIO_BUFFER_OFFSET,
          (unsigned)AUDIO_FILL_BYTES_PER_VBL, (unsigned)AUDIO_FIFO_SLICES);
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

static void audio_yms_cb(uint8_t *buf, uint32_t bytes) {
  UINT br = 0;
  FRESULT res = f_read(&s_yms_file, buf, bytes, &br);
  if (res != FR_OK) {
    if (!s_yms_failing) {
      DPRINTF("audio: .YMS read failed (%d), playing silence\n", (int)res);
      s_yms_failing = true;
    }
    /* I/O error -- silence until the next call. The cursor is in
     * an undefined state, so seek back to the data start for the
     * next attempt. */
    for (uint32_t i = 0; i < bytes; i++) {
      buf[i] = 0;
    }
    f_lseek(&s_yms_file, s_yms_data_offset);
    return;
  }
  s_yms_failing = false;
  if (br < bytes) {
    /* EOF mid-fill: wrap to data start and read the remainder. */
    f_lseek(&s_yms_file, s_yms_data_offset);
    UINT br2 = 0;
    f_read(&s_yms_file, buf + br, bytes - br, &br2);
    /* If the file body is shorter than one VBL's worth, pad the
     * still-unfilled tail with silence rather than reading the
     * header bytes. */
    for (uint32_t i = br + br2; i < bytes; i++) {
      buf[i] = 0;
    }
  }
}

int audio_play_yms_file(const char *path) {
  /* Close any previously open YMS file. Idempotent on a fresh init. */
  if (s_yms_open) {
    f_close(&s_yms_file);
    s_yms_open = false;
  }

  FRESULT res = f_open(&s_yms_file, path, FA_READ);
  if (res != FR_OK) {
    DPRINTF("audio_play_yms_file: f_open('%s') failed (%d)\n", path, (int)res);
    return -1;
  }

  uint8_t hdr[AUDIO_YMS_HEADER_SIZE];
  UINT br = 0;
  res = f_read(&s_yms_file, hdr, sizeof(hdr), &br);
  if (res != FR_OK || br != sizeof(hdr)) {
    DPRINTF("audio_play_yms_file: short header read (%d, %u/%u)\n",
            (int)res, (unsigned)br, (unsigned)sizeof(hdr));
    f_close(&s_yms_file);
    return -1;
  }

  if (memcmp(hdr, "YMS1", 4) != 0) {
    DPRINTF("audio_play_yms_file: bad magic %02X%02X%02X%02X\n",
            hdr[0], hdr[1], hdr[2], hdr[3]);
    f_close(&s_yms_file);
    return -1;
  }

  uint32_t rate = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
  if (rate != AUDIO_NATIVE_RATE_HZ) {
    DPRINTF("audio_play_yms_file: rate mismatch (file %lu, expected %u)\n",
            (unsigned long)rate, (unsigned)AUDIO_NATIVE_RATE_HZ);
    f_close(&s_yms_file);
    return -1;
  }

  if (hdr[12] != AUDIO_YMS_MODE_DUAL_GHOST) {
    DPRINTF("audio_play_yms_file: unsupported mode tag %u (need %u)\n",
            (unsigned)hdr[12], (unsigned)AUDIO_YMS_MODE_DUAL_GHOST);
    f_close(&s_yms_file);
    return -1;
  }

  s_yms_open = true;
  s_yms_data_offset = AUDIO_YMS_HEADER_SIZE;
  s_fill_cb = audio_yms_cb;

  DPRINTF("audio_play_yms_file: '%s' streaming (rate %lu Hz, mode %u)\n",
          path, (unsigned long)rate, (unsigned)hdr[12]);
  return 0;
}

/* Write the slice for the ST's VBL number `vbl`: one VBL of samples from
 * the FIFO, then the last sample held to the end of the slice (Timer-B may
 * read a sample past a VBL's worth). An empty FIFO holds the last sample
 * over the whole slice. */
static void __not_in_flash_func(audio_write_slice)(uint32_t vbl) {
  uint8_t *slice =
      s_audio_buf + (vbl % CART_AUDIO_SLICES) * CART_AUDIO_SLICE_BYTES;
  uint32_t filled = 0;
  uint32_t tail = s_fifo_tail;
  if (tail != s_fifo_head) {
    const uint8_t *src = s_fifo[tail % AUDIO_FIFO_SLICES];
    memcpy(slice, src, AUDIO_FILL_BYTES_PER_VBL);
    s_hold[0] = src[AUDIO_FILL_BYTES_PER_VBL - 2u];
    s_hold[1] = src[AUDIO_FILL_BYTES_PER_VBL - 1u];
    s_fifo_tail = tail + 1u;
    filled = AUDIO_FILL_BYTES_PER_VBL;
  } else if (s_fill_cb != NULL) {
    audioUnderruns++;
  }
  for (uint32_t i = filled; i < CART_AUDIO_SLICE_BYTES; i += 2u) {
    slice[i] = s_hold[0];
    slice[i + 1u] = s_hold[1];
  }
  audioSlicesWritten++;
}

/* The slice writer, every AUDIO_WRITER_PERIOD_US in a timer interrupt: find
 * the slice the ST plays from its latest report, and write the
 * AUDIO_SLICES_AHEAD slices after it. Everything it touches is in RAM. */
static bool __not_in_flash_func(audio_writer)(repeating_timer_t *timer) {
  (void)timer;
  uint16_t report;
  if (!commemul_latest(CART_ROM3_WINDOW_MASK, CART_ROM3_AUDIO_SLICE_WINDOW,
                       AUDIO_REPORT_LOOKBACK, &report)) {
    return true; /* the ST is not playing: nothing to follow */
  }
  uint32_t now_us = time_us_32();
  if (report != s_last_report) {
    s_last_report = report;
    s_last_report_us = now_us;
  } else if (now_us - s_last_report_us > AUDIO_REPORT_TIMEOUT_US) {
    s_st_known = false; /* stale: the ST stopped playing */
    return true;
  }

  uint32_t slice = report & (CART_AUDIO_SLICES - 1u);
  if (!s_st_known) {
    s_st_known = true;
    s_st_vbl = slice;
    s_next_vbl = slice + 1u;
  } else {
    /* The report is the slice modulo CART_AUDIO_SLICES: a writer stalled for
     * that many VBLs or more (80 ms with the interrupts off) cannot tell, and
     * counts no late slices for it. The slices still go where they should. */
    s_st_vbl += (slice - s_st_vbl) % CART_AUDIO_SLICES;
  }
  if (s_next_vbl <= s_st_vbl) {
    /* The ST started a slice that was never written for it. */
    audioLateSlices += s_st_vbl + 1u - s_next_vbl;
    s_next_vbl = s_st_vbl + 1u;
  }
  while (s_next_vbl <= s_st_vbl + AUDIO_SLICES_AHEAD) {
    audio_write_slice(s_next_vbl);
    s_next_vbl++;
  }
  return true;
}

void audio_render_frame(void) {
  if (s_fill_cb == NULL) {
    return;
  }
  /* Top up the FIFO: the writer reads only the slices below s_fifo_head. */
  while (s_fifo_head - s_fifo_tail < AUDIO_FIFO_SLICES) {
    s_fill_cb(s_fifo[s_fifo_head % AUDIO_FIFO_SLICES],
              AUDIO_FILL_BYTES_PER_VBL);
    __dmb();
    s_fifo_head = s_fifo_head + 1u;
  }
}
