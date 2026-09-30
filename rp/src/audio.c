/**
 * File: audio.c
 * Description: The app's sound, to the YM or to the DMA sound chip.
 *
 * Two outputs, one per machine (the ST's hello says which):
 *   - the YM: the m68k Timer-B IRQ plays (vA, vB) pairs from the cart buffer
 *     at CART_AUDIO_BUFFER_OFFSET, one slice of CART_AUDIO_SLICES per VBL:
 *     the ST's VBL handler moves Timer-B to the next slice and reports which
 *     one through CART_ROM3_AUDIO_SLICE_WINDOW. The RP writes only the slices
 *     ahead of the one playing, so the ST never plays a slice while it is
 *     being written;
 *   - the DMA sound chip of an STE or a Mega STE: it plays 8-bit samples at
 *     the profile's rate from a ring in ST RAM, which the ST fills every VBL
 *     from the cart buffer, its mirror, up to the profile's lead
 *     (PROFILE_DMA_LEAD) ahead of where it reports the chip plays
 *     (CART_ROM3_DMA_POS_WINDOW). The RP writes the mirror further ahead
 *     still, never where the ST copies.
 * The rates are the app's profile's (profile.h): the YM at 5,585 or
 * 21,943 Hz, the DMA chip at 12,517 or 25,033 Hz.
 * The ST picks the output at boot by the same rule the RP applies at the
 * hello (select_output): the DMA chip when the machine has one, unless the
 * app asked for the YM (audio_prefer_ym; CART_AUDIO_OUT_OFFSET).
 *
 * Two stages, so that the sound does not depend on the app's frame loop:
 *   - audio_render_frame(), from the main loop, tops up a small RAM FIFO in
 *     the output's format with the app's source: YM pairs (a loop in flash, a
 *     .YMS file on the SD card, the app's callback) or 8-bit PCM (a loop, a
 *     callback), converted when the output wants the other (the 64-entry
 *     two-channel table both ways, linear resampling between the rates);
 *   - a repeating timer interrupt (audio_writer) follows the ST's reports and
 *     moves samples from the FIFO into the cart buffer ahead of the ST.
 * The main loop may go about AUDIO_FIFO_SLICES VBLs between two top-ups
 * without the sound noticing (80 ms at 50 fps, 60 at 25). When the FIFO is empty the
 * output holds the last sample (an underrun, counted), never stale data.
 *
 * See audio.h for the public API.
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
#include "profile.h"
#include "st_session.h"

/* A PAL VBL, nominally. The slices follow the ST's own VBLs; this is the
 * period the sample arithmetic below is for. */
#define AUDIO_FRAME_PERIOD_US 20000u

/* One VBL of samples: what Timer-B plays between two VBLs, two bytes a
 * sample: 224 at 5,585 Hz (111.7 samples), 878 at 21,943 Hz (438.2), of a
 * slice's CART_AUDIO_SLICE_BYTES (the profile, profile.h). */
#define AUDIO_FILL_BYTES_PER_VBL PROFILE_YM_BYTES_PER_VBL
#define AUDIO_FILL_SAMPLES_PER_VBL (AUDIO_FILL_BYTES_PER_VBL / 2u)

/* Slices written ahead of the one the ST plays. */
#define AUDIO_SLICES_AHEAD 2u

/* The FIFO between the app's source and the writer, in VBLs of samples:
 * the longest the main loop may go between two top-ups. More delays the
 * sound as much: on the YM up to AUDIO_FIFO_SLICES + AUDIO_SLICES_AHEAD
 * VBLs from the source to the ST. At 25 fps a VBL of samples is four times
 * larger and the main loop tops up every 40 ms: 3 VBLs, for the RAM. */
#ifndef AUDIO_FIFO_SLICES
#if APP_PROFILE == PROFILE_25FPS
#define AUDIO_FIFO_SLICES 3u
#else
#define AUDIO_FIFO_SLICES 4u
#endif
#endif

/* The writer runs this often; the ST's report is seen at most this long
 * after its VBL, well before the samples are needed. */
#define AUDIO_WRITER_PERIOD_US 4000
/* How far back in the ROM3 ring the writer looks for the latest report: a
 * frame adds a few samples (the reports, the blit acknowledgement, keys). */
#define AUDIO_REPORT_LOOKBACK 64u
/* No new report for this long: the ST left userfw (GEM, a reset). The next
 * report starts the count again. */
#define AUDIO_REPORT_TIMEOUT_US 200000u

/* The YM output's rate: Timer-B's, set by userfw from the profile. */
#define AUDIO_NATIVE_RATE_HZ      PROFILE_YM_RATE_HZ

/* The rate of YM-pair sources: .YMS files, the built-in jingle, fill
 * callbacks. At another output rate they are resampled like PCM. */
#define AUDIO_YM_SOURCE_RATE_HZ   5585u

/* The DMA output: its FIFO holds five VBLs of samples (250.3 a VBL at
 * 12,517 Hz, 500.7 at 25,033): it is topped up by the chunk, and the main
 * loop may still go four VBLs between two top-ups, in both profiles (at
 * 25 fps it shares the YM FIFO's memory, which is larger still). The
 * writer keeps the mirror written this far ahead of where
 * the chip plays now: its last report, plus the samples played since (a
 * missed VBL sends no report, and the chip goes on). At its next VBL the ST
 * copies up to PROFILE_DMA_LEAD ahead; the rest covers the writer's period
 * and how late it sees a report (4 ms each: 5/8 of a VBL together). */
#define AUDIO_DMA_FIFO_BYTES (5u * PROFILE_DMA_BYTES_PER_VBL)
#define AUDIO_DMA_AHEAD (PROFILE_DMA_LEAD + PROFILE_DMA_BYTES_PER_VBL * 5u / 8u)
/* The estimate goes at most this far past the last report. */
#define AUDIO_DMA_EST_MAX_US (3u * AUDIO_FRAME_PERIOD_US)
#define AUDIO_DMA_RING_MASK (CART_AUDIO_DMA_RING_BYTES - 1u)


/* A PCM callback is asked for this many samples at a time. */
#define AUDIO_PCM_CHUNK 64u

typedef enum {
  AUDIO_OUT_NONE = 0, /* between two outputs: the writer waits */
  AUDIO_OUT_YM,
  AUDIO_OUT_DMA,
} audio_out_t;

static uint8_t *s_audio_buf;
static volatile audio_out_t s_out;
static bool s_prefer_ym;
static uint32_t s_hellos_seen;

/* The app's source: YM pairs at AUDIO_YM_SOURCE_RATE_HZ, or PCM at a rate. */
static audio_fill_cb_t s_fill_cb;
static audio_pcm_cb_t s_pcm_cb;
static uint32_t s_pcm_rate;

/* The output's FIFO: whole VBLs of pairs for the YM, samples for the DMA
 * chip. One output plays at a time, so they share the memory. */
static union {
  uint8_t ym[AUDIO_FIFO_SLICES][AUDIO_FILL_BYTES_PER_VBL];
  int8_t dma[AUDIO_DMA_FIFO_BYTES];
} s_fifo_mem;
#define s_fifo (s_fifo_mem.ym)
#define s_dma_fifo (s_fifo_mem.dma)
static volatile uint32_t s_fifo_head; /* YM slices produced (main loop) */
static volatile uint32_t s_fifo_tail; /* slices consumed (writer) */
static volatile uint32_t s_dma_head; /* samples produced (main loop) */
static volatile uint32_t s_dma_tail; /* samples consumed (writer) */

/* The YM writer's view of the ST. */
static bool s_st_known;      /* the ST's slice is known */
static uint32_t s_st_vbl;    /* VBLs counted from the reports; its low bits are the slice */
static uint32_t s_next_vbl;  /* the next VBL whose slice is not written yet */
static uint16_t s_last_report;
static uint32_t s_last_report_us;
static uint8_t s_hold[2];    /* the last sample written: what an underrun holds */

/* The DMA writer's view of the ST. */
static bool s_dma_known;
static uint32_t s_dma_front; /* the next offset of the ring the RP writes */
static int8_t s_dma_hold;

static repeating_timer_t s_writer_timer;

/* Readable over SWD by their symbols (tools/dev/swd.py counters). */
uint32_t audioSlicesWritten; /* VBLs of samples the writer followed */
uint32_t audioUnderruns;     /* writes of a held sample: the FIFO was empty */
uint32_t audioLateSlices;    /* the ST played or copied what the RP had not written */
uint32_t audioOutput;        /* 1: the YM, 2: the DMA sound chip */

static bool audio_writer(repeating_timer_t *timer);

/* Static-loop convenience state. audio_play_loop() points
 * s_fill_cb at audio_loop_cb and stores the source span here. */
static const uint8_t *s_loop_data;
static uint32_t s_loop_bytes;
static uint32_t s_loop_pos;

/* audio_play_pcm_loop()'s span. */
static const int8_t *s_pcm_loop_data;
static uint32_t s_pcm_loop_samples;
static uint32_t s_pcm_loop_pos;

/* .YMS-file streaming state. audio_play_yms_file() validates the
 * 16-byte header, stores the data offset, and installs audio_yms_cb
 * as the per-VBL fill callback. The file is kept open for the
 * lifetime of playback. */
#define AUDIO_YMS_HEADER_SIZE     16u
#define AUDIO_YMS_MODE_DUAL_GHOST 1u

static FIL s_yms_file;
static bool s_yms_open;
/* A read failed and has not succeeded since: logged once per streak. */
static bool s_yms_failing;
static FSIZE_t s_yms_data_offset;

/* --- PCM <-> YM pairs ---------------------------------------------------- */

/* The (vA, vB) pairs of the 1988 Ghostbusters demo's SAMPLE1 table, indexed
 * by the upper 6 bits of an unsigned 8-bit sample: tools/wav_to_ym4.py's
 * GHOSTBUSTERS_LUT (tests/host/test_audio_convert.c compares the two). */
static const uint8_t k_ghost_pairs[64][2] = {
    {0, 0},  {0, 2},  {1, 2},  {2, 2},  {2, 3},  {1, 4},  {2, 4},  {2, 5},
    {0, 6},  {2, 6},  {3, 6},  {4, 6},  {2, 7},  {4, 7},  {5, 7},  {2, 8},
    {3, 8},  {4, 8},  {5, 8},  {2, 9},  {3, 9},  {4, 9},  {5, 9},  {6, 9},
    {7, 9},  {3, 10}, {4, 10}, {5, 10}, {6, 10}, {7, 10}, {0, 11}, {1, 11},
    {2, 11}, {4, 11}, {5, 11}, {6, 11}, {7, 11}, {8, 11}, {8, 11}, {9, 11},
    {9, 11}, {0, 12}, {1, 12}, {2, 12}, {3, 12}, {4, 12}, {5, 12}, {6, 12},
    {8, 12}, {8, 12}, {9, 12}, {9, 12}, {9, 12}, {10, 12}, {0, 13}, {2, 13},
    {3, 13}, {4, 13}, {5, 13}, {6, 13}, {7, 13}, {8, 13}, {8, 13}, {9, 13},
};

/* A YM channel's amplitude at each volume, x 10,000: 2^((v - 15) / 2), the
 * curve tools/wav_to_ym4.py fits pairs to. */
static const uint16_t k_ym_amp[16] = {0,    78,   110,  156,  221,  313,
                                      442,  625,  884,  1250, 1768, 2500,
                                      3536, 5000, 7071, 10000};

/* A pair (vA << 4 | vB) as a signed 8-bit sample: the middle of the table
 * entries it is, else the entry nearest in summed amplitude. */
static int8_t s_pair_pcm[256];

static int8_t pcm_of_index(uint32_t index) {
  return (int8_t)((int32_t)(index * 4u + 2u) - 128);
}

static void build_pair_pcm(void) {
  for (uint32_t pair = 0; pair < 256u; pair++) {
    uint32_t a = pair >> 4, b = pair & 15u;
    uint32_t sum = 0, count = 0;
    for (uint32_t i = 0; i < 64u; i++) {
      if (k_ghost_pairs[i][0] == a && k_ghost_pairs[i][1] == b) {
        sum += i;
        count++;
      }
    }
    if (count != 0u) {
      s_pair_pcm[pair] = pcm_of_index((sum + count / 2u) / count);
      continue;
    }
    uint32_t amp = k_ym_amp[a] + k_ym_amp[b];
    uint32_t best = 0, best_diff = UINT32_MAX;
    for (uint32_t i = 0; i < 64u; i++) {
      uint32_t e = k_ym_amp[k_ghost_pairs[i][0]] + k_ym_amp[k_ghost_pairs[i][1]];
      uint32_t diff = e > amp ? e - amp : amp - e;
      if (diff < best_diff) {
        best_diff = diff;
        best = i;
      }
    }
    s_pair_pcm[pair] = pcm_of_index(best);
  }
}

/* --- The source, as a stream of samples ---------------------------------- */

/* YM pairs from a fill callback, a chunk at a time: any size will do. */
#define AUDIO_YM_STAGE_BYTES 256u
static uint8_t s_ym_stage[AUDIO_YM_STAGE_BYTES];
static uint32_t s_ym_stage_pos = AUDIO_YM_STAGE_BYTES;
static int8_t s_pcm_stage[AUDIO_PCM_CHUNK];
static uint32_t s_pcm_stage_pos = AUDIO_PCM_CHUNK;

/* Linear resampling from the source's rate: s0 and s1 are the source
 * samples around the output sample, pos the fraction between them (16.16). */
static bool s_rs_primed;
static int32_t s_rs_s0;
static int32_t s_rs_s1;
static uint32_t s_rs_pos;

static void source_restart(void) {
  s_ym_stage_pos = AUDIO_YM_STAGE_BYTES;
  s_pcm_stage_pos = AUDIO_PCM_CHUNK;
  s_rs_primed = false;
}

static uint32_t source_rate(void) {
  return s_pcm_cb != NULL ? s_pcm_rate : AUDIO_YM_SOURCE_RATE_HZ;
}

/* The next source sample, as signed 8-bit PCM. */
static int32_t source_sample(void) {
  if (s_fill_cb != NULL) {
    if (s_ym_stage_pos >= AUDIO_YM_STAGE_BYTES) {
      s_fill_cb(s_ym_stage, AUDIO_YM_STAGE_BYTES);
      s_ym_stage_pos = 0;
    }
    uint32_t pair = ((s_ym_stage[s_ym_stage_pos] & 15u) << 4) |
                    (s_ym_stage[s_ym_stage_pos + 1u] & 15u);
    s_ym_stage_pos += 2u;
    return s_pair_pcm[pair];
  }
  if (s_pcm_cb != NULL) {
    if (s_pcm_stage_pos >= AUDIO_PCM_CHUNK) {
      s_pcm_cb(s_pcm_stage, AUDIO_PCM_CHUNK);
      s_pcm_stage_pos = 0;
    }
    return s_pcm_stage[s_pcm_stage_pos++];
  }
  return 0;
}

/* The next output sample at `step` source samples per output sample
 * (16.16): 1.0 passes the source through unchanged. */
static int32_t resample(uint32_t step) {
  if (!s_rs_primed) {
    s_rs_s0 = source_sample();
    s_rs_s1 = source_sample();
    s_rs_pos = 0;
    s_rs_primed = true;
  }
  int32_t v = s_rs_s0 + (((s_rs_s1 - s_rs_s0) * (int32_t)(s_rs_pos >> 4)) >> 12);
  s_rs_pos += step;
  while (s_rs_pos >= 0x10000u) {
    s_rs_pos -= 0x10000u;
    s_rs_s0 = s_rs_s1;
    s_rs_s1 = source_sample();
  }
  return v;
}

static uint32_t rate_step(uint32_t out_rate) {
  return (uint32_t)(((uint64_t)source_rate() << 16) / out_rate);
}

/* One VBL of YM pairs. A YM source at the output's rate goes through as it
 * is. */
static void produce_ym(uint8_t *dst) {
  if (s_fill_cb != NULL && AUDIO_YM_SOURCE_RATE_HZ == AUDIO_NATIVE_RATE_HZ) {
    s_fill_cb(dst, AUDIO_FILL_BYTES_PER_VBL);
    return;
  }
  uint32_t step = rate_step(AUDIO_NATIVE_RATE_HZ);
  for (uint32_t i = 0; i < AUDIO_FILL_SAMPLES_PER_VBL; i++) {
    uint32_t index = (uint32_t)(resample(step) + 128) >> 2;
    dst[2u * i] = k_ghost_pairs[index][0];
    dst[2u * i + 1u] = k_ghost_pairs[index][1];
  }
}

/* `n` samples for the DMA chip. */
static void produce_pcm(int8_t *dst, uint32_t n) {
  uint32_t step = rate_step(PROFILE_DMA_RATE_HZ);
  for (uint32_t i = 0; i < n; i++) dst[i] = (int8_t)resample(step);
}

/* --- The outputs ----------------------------------------------------------- */

static void write_audio_out(void) {
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_AUDIO_OUT_OFFSET)) =
      (uint16_t)(s_prefer_ym ? CART_AUDIO_OUT_YM : CART_AUDIO_OUT_AUTO);
}

/* The output for the ST that just said hello: the ST applies the same rule
 * (userfw.s, .sound_ym). The writer waits while the state starts over. */
static void select_output(uint8_t machine) {
  audio_out_t out = (!s_prefer_ym && (machine >> 4) == 1u) ? AUDIO_OUT_DMA
                                                           : AUDIO_OUT_YM;
  s_out = AUDIO_OUT_NONE;
  __dmb();
  s_fifo_head = s_fifo_tail = 0;
  s_dma_head = s_dma_tail = 0;
  s_st_known = false;
  s_dma_known = false;
  s_hold[0] = s_hold[1] = 0;
  s_dma_hold = 0;
  source_restart();
  /* The ST copies the mirror before the RP has seen its first report: let
   * that be silence, not the last session's sound. */
  memset(s_audio_buf, 0, CART_AUDIO_BUFFER_SIZE);
  __dmb();
  s_out = out;
  audioOutput = out == AUDIO_OUT_DMA ? 2u : 1u;
  DPRINTF("audio: machine $%02X, %s, %u Hz (%s)\n", (unsigned)machine,
          out == AUDIO_OUT_DMA ? "DMA sound" : "YM",
          (unsigned)(out == AUDIO_OUT_DMA ? PROFILE_DMA_RATE_HZ
                                          : AUDIO_NATIVE_RATE_HZ),
          PROFILE_NAME);
}

void audio_init(void) {
  uint8_t *base = (uint8_t *)&__rom_in_ram_start__;
  s_audio_buf = base + CART_AUDIO_BUFFER_OFFSET;
  s_fill_cb = NULL;
  s_pcm_cb = NULL;
  s_yms_open = false;
  build_pair_pcm();
  write_audio_out();
  s_hellos_seen = st_session_hellos();
  select_output(st_session_machine());

  add_repeating_timer_us(-AUDIO_WRITER_PERIOD_US, audio_writer, NULL,
                         &s_writer_timer);

  DPRINTF("audio_init: %u B at offset $%04X; YM %u slices of %u B, "
          "FIFO %u VBLs\n",
          (unsigned)CART_AUDIO_BUFFER_SIZE, (unsigned)CART_AUDIO_BUFFER_OFFSET,
          (unsigned)CART_AUDIO_SLICES, (unsigned)CART_AUDIO_SLICE_BYTES,
          (unsigned)AUDIO_FIFO_SLICES);
}

void audio_prefer_ym(bool prefer) {
  s_prefer_ym = prefer;
  write_audio_out();
  DPRINTF("audio: %s from the next ST boot\n",
          prefer ? "the YM" : "the DMA chip where there is one");
}

bool audio_uses_dma(void) { return s_out == AUDIO_OUT_DMA; }

/* --- Sources ----------------------------------------------------------------- */

void audio_set_fill_callback(audio_fill_cb_t cb) {
  s_pcm_cb = NULL;
  s_fill_cb = cb;
  source_restart();
}

void audio_set_pcm_callback(audio_pcm_cb_t cb, uint32_t rate_hz) {
  s_fill_cb = NULL;
  s_pcm_rate = rate_hz != 0u ? rate_hz : PROFILE_DMA_RATE_HZ;
  s_pcm_cb = cb;
  source_restart();
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
  audio_set_fill_callback(audio_loop_cb);
}

static void audio_pcm_loop_cb(int8_t *buf, uint32_t samples) {
  for (uint32_t i = 0; i < samples; i++) {
    buf[i] = s_pcm_loop_data[s_pcm_loop_pos];
    if (++s_pcm_loop_pos >= s_pcm_loop_samples) s_pcm_loop_pos = 0;
  }
}

void audio_play_pcm_loop(const int8_t *pcm, uint32_t samples, uint32_t rate_hz) {
  if (pcm == NULL || samples == 0u) {
    audio_set_fill_callback(NULL);
    return;
  }
  s_pcm_loop_data = pcm;
  s_pcm_loop_samples = samples;
  s_pcm_loop_pos = 0;
  audio_set_pcm_callback(audio_pcm_loop_cb, rate_hz);
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
  if (rate != AUDIO_YM_SOURCE_RATE_HZ) {
    DPRINTF("audio_play_yms_file: rate mismatch (file %lu, expected %u)\n",
            (unsigned long)rate, (unsigned)AUDIO_YM_SOURCE_RATE_HZ);
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
  audio_set_fill_callback(audio_yms_cb);

  DPRINTF("audio_play_yms_file: '%s' streaming (rate %lu Hz, mode %u)\n",
          path, (unsigned long)rate, (unsigned)hdr[12]);
  return 0;
}

/* --- The writers (timer interrupt) -------------------------------------------- */

static bool source_set(void) { return s_fill_cb != NULL || s_pcm_cb != NULL; }

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
  } else if (source_set()) {
    audioUnderruns++;
  }
  for (uint32_t i = filled; i < CART_AUDIO_SLICE_BYTES; i += 2u) {
    slice[i] = s_hold[0];
    slice[i + 1u] = s_hold[1];
  }
  audioSlicesWritten++;
}

/* The latest report in `window`, and whether it is new since the last
 * call (`fresh`). */
typedef enum {
  REPORT_NONE,  /* none in the ring: the ST is not playing */
  REPORT_STALE, /* none new for AUDIO_REPORT_TIMEOUT_US: the ST left userfw */
  REPORT_OK,
} report_t;

static report_t __not_in_flash_func(latest_report)(uint16_t window,
                                                   uint16_t *report,
                                                   bool *fresh) {
  if (!commemul_latest(CART_ROM3_WINDOW_MASK, window, AUDIO_REPORT_LOOKBACK,
                       report)) {
    return REPORT_NONE;
  }
  uint32_t now_us = time_us_32();
  *fresh = *report != s_last_report;
  if (*fresh) {
    s_last_report = *report;
    s_last_report_us = now_us;
  } else if (now_us - s_last_report_us > AUDIO_REPORT_TIMEOUT_US) {
    return REPORT_STALE;
  }
  return REPORT_OK;
}

/* The YM: find the slice the ST plays from its latest report, and write the
 * AUDIO_SLICES_AHEAD slices after it. */
static void __not_in_flash_func(ym_writer)(void) {
  uint16_t report;
  bool fresh;
  report_t r = latest_report(CART_ROM3_AUDIO_SLICE_WINDOW, &report, &fresh);
  if (r != REPORT_OK) {
    if (r == REPORT_STALE) s_st_known = false; /* the ST stopped playing */
    return;
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
}

/* The DMA chip: at each report the ST has just copied the mirror up to
 * PROFILE_DMA_LEAD ahead of where the chip plays; keep writing it
 * AUDIO_DMA_AHEAD ahead of where it plays now, from the FIFO or, when that
 * is empty, the last sample held. Byte i of the ring is byte i ^ 1 of the
 * buffer. */
static void __not_in_flash_func(dma_writer)(void) {
  uint16_t report;
  bool fresh;
  report_t r = latest_report(CART_ROM3_DMA_POS_WINDOW, &report, &fresh);
  if (r != REPORT_OK) {
    if (r == REPORT_STALE) s_dma_known = false;
    return;
  }
  uint32_t play = (report & 0xFFu) * CART_AUDIO_DMA_POS_UNIT;
  if (!s_dma_known) {
    s_dma_known = true;
    s_dma_front = (play + PROFILE_DMA_LEAD) & AUDIO_DMA_RING_MASK;
  } else if (fresh) {
    audioSlicesWritten++;
    uint32_t ahead = (s_dma_front - play) & AUDIO_DMA_RING_MASK;
    if (ahead < PROFILE_DMA_LEAD || ahead > CART_AUDIO_DMA_RING_BYTES / 2u) {
      /* The ST copied what the RP had not written yet. */
      audioLateSlices++;
      s_dma_front = (play + PROFILE_DMA_LEAD) & AUDIO_DMA_RING_MASK;
    }
  }
  uint32_t since_us = time_us_32() - s_last_report_us;
  if (since_us > AUDIO_DMA_EST_MAX_US) since_us = AUDIO_DMA_EST_MAX_US;
  uint32_t now_at = play + since_us * PROFILE_DMA_RATE_HZ / 1000000u;
  uint32_t target = (now_at + AUDIO_DMA_AHEAD) & AUDIO_DMA_RING_MASK;
  uint32_t n = (target - s_dma_front) & AUDIO_DMA_RING_MASK;
  if (n > CART_AUDIO_DMA_RING_BYTES / 2u) return; /* already that far */
  bool held = false;
  uint32_t front = s_dma_front;
  uint32_t tail = s_dma_tail;
  uint32_t head = s_dma_head;
  for (uint32_t i = 0; i < n; i++) {
    if (tail != head) {
      s_dma_hold = s_dma_fifo[tail % AUDIO_DMA_FIFO_BYTES];
      tail++;
    } else {
      held = true;
    }
    s_audio_buf[front ^ 1u] = (uint8_t)s_dma_hold;
    front = (front + 1u) & AUDIO_DMA_RING_MASK;
  }
  s_dma_tail = tail;
  s_dma_front = front;
  if (held && source_set()) audioUnderruns++;
}

/* Every AUDIO_WRITER_PERIOD_US in a timer interrupt. Everything it touches
 * is in RAM. */
static bool __not_in_flash_func(audio_writer)(repeating_timer_t *timer) {
  (void)timer;
  audio_out_t out = s_out;
  if (out == AUDIO_OUT_YM) {
    ym_writer();
  } else if (out == AUDIO_OUT_DMA) {
    dma_writer();
  }
  return true;
}

/* --- The main loop's part ------------------------------------------------------ */

void audio_render_frame(void) {
  /* A new ST session: the output it plays through. */
  uint32_t hellos = st_session_hellos();
  if (hellos != s_hellos_seen) {
    s_hellos_seen = hellos;
    select_output(st_session_machine());
  }
  if (!source_set()) {
    return;
  }
  if (s_out == AUDIO_OUT_DMA) {
    /* Top up the FIFO a chunk at a time: the writer reads only the samples
     * below s_dma_head. */
    while (AUDIO_DMA_FIFO_BYTES - (s_dma_head - s_dma_tail) >= AUDIO_PCM_CHUNK) {
      int8_t chunk[AUDIO_PCM_CHUNK];
      produce_pcm(chunk, AUDIO_PCM_CHUNK);
      uint32_t head = s_dma_head;
      for (uint32_t i = 0; i < AUDIO_PCM_CHUNK; i++) {
        s_dma_fifo[(head + i) % AUDIO_DMA_FIFO_BYTES] = chunk[i];
      }
      __dmb();
      s_dma_head = head + AUDIO_PCM_CHUNK;
    }
    return;
  }
  /* Top up the FIFO: the writer reads only the slices below s_fifo_head. */
  while (s_fifo_head - s_fifo_tail < AUDIO_FIFO_SLICES) {
    produce_ym(s_fifo[s_fifo_head % AUDIO_FIFO_SLICES]);
    __dmb();
    s_fifo_head = s_fifo_head + 1u;
  }
}
