// units: rp/src/audio.c
/* The DMA sound output (audio.c) against a simulated STE, and the
 * conversions between YM pairs and PCM.
 *
 * The STE plays its ring at the profile's rate (250.3 samples a VBL at
 * 12,517 Hz, 500.7 at 25,033: the Makefile builds this test for both
 * profiles); at every VBL userfw reports where it plays, rounded to
 * CART_AUDIO_DMA_POS_UNIT bytes, and copies the cart buffer, the ring's
 * mirror, up to the profile's lead ahead (byte i of the ring is byte i ^ 1
 * of the RP's buffer). Everything the chip plays
 * must be the source in order, silence before it starts, or, when the FIFO
 * ran dry, the last sample held: never a stale or a skipped sample. */
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "ff.h"
#include "pico/time.h"
#include "test.h"

#define VBL_US 20000u
#define WRITER_RUNS_PER_VBL 5
#define RING CART_AUDIO_DMA_RING_BYTES
#define MASK (RING - 1u)
/* The chip's samples per VBL, 16.16. */
#define CHIP_PER_VBL_Q16 \
  ((uint32_t)(((uint64_t)PROFILE_DMA_RATE_HZ * VBL_US << 16) / 1000000u))
/* Five VBLs of the chip's samples: the sound starts within them. */
#define FIVE_VBLS (5u * ((CHIP_PER_VBL_Q16 >> 16) + 1u))

unsigned int __rom_in_ram_start__[0x10000 / sizeof(unsigned int)];
extern uint32_t audioSlicesWritten, audioUnderruns, audioLateSlices, audioOutput;

/* --- The RP's world ------------------------------------------------------- */

static uint32_t now_us;
static repeating_timer_t *writer;
static int dma_report = -1;   /* the latest DMA position report */
static int slice_report = -1; /* the latest audio slice report */
static uint32_t hellos;
static uint8_t machine;

uint32_t time_us_32(void) { return now_us; }
uint32_t st_session_hellos(void) { return hellos; }
uint8_t st_session_machine(void) { return machine; }

bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t callback,
                            void *user_data, repeating_timer_t *out) {
  out->delay_us = delay_us;
  out->callback = callback;
  out->user_data = user_data;
  writer = out;
  return true;
}

bool commemul_latest(uint16_t mask, uint16_t match, uint32_t max_back,
                     uint16_t *sample) {
  (void)max_back;
  if ((match & mask) == CART_ROM3_DMA_POS_WINDOW && dma_report >= 0) {
    *sample = (uint16_t)(CART_ROM3_DMA_POS_WINDOW | (uint16_t)dma_report);
    return true;
  }
  if ((match & mask) == CART_ROM3_AUDIO_SLICE_WINDOW && slice_report >= 0) {
    *sample = (uint16_t)(CART_ROM3_AUDIO_SLICE_WINDOW | (uint16_t)slice_report);
    return true;
  }
  return false;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)fp, (void)path, (void)mode;
  return FR_NO_FILE;
}
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  (void)fp, (void)buff, (void)btr;
  *br = 0;
  return FR_DISK_ERR;
}
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  (void)fp, (void)ofs;
  return FR_OK;
}
FRESULT f_close(FIL *fp) {
  (void)fp;
  return FR_OK;
}

static uint8_t *cart_buffer(void) {
  return (uint8_t *)__rom_in_ram_start__ + CART_AUDIO_BUFFER_OFFSET;
}

/* --- The STE ------------------------------------------------------------------ */

static int8_t ring[RING];      /* ST RAM */
static uint32_t chip_q16;      /* samples the chip has played, 16.16 */
static uint32_t st_front;      /* UFW_DMA_FRONT */
static uint32_t st_vbl;
static int8_t played[400000];
static uint32_t n_played;

/* A boot of the ST: hello, ring cleared, the chip restarted. */
static void st_boot(uint8_t m) {
  machine = m;
  hellos++;
  audio_render_frame(); /* the RP sees the hello */
  memset(ring, 0, sizeof ring);
  chip_q16 = 0;
  st_front = 0;
  dma_report = -1;
  slice_report = -1;
}

/* userfw at a VBL on the DMA path: report the play position, copy the
 * mirror ahead. `skip`: a blit that overran, and .vbl_loop missed this VBL
 * entirely: no report, no copy. */
static void st_vbl_dma(bool skip) {
  st_vbl++;
  if (skip) return;
  slice_report = (int)(st_vbl % CART_AUDIO_SLICES);
  uint32_t p = (chip_q16 >> 16) & MASK & ~(CART_AUDIO_DMA_POS_UNIT - 1u);
  dma_report = (int)(p / CART_AUDIO_DMA_POS_UNIT);
  uint32_t e = (p + PROFILE_DMA_LEAD) & MASK;
  uint32_t f = st_front;
  uint32_t n = (e - f) & MASK;
  if (n > RING / 2u) {
    f = (e - PROFILE_DMA_LEAD) & MASK;
    n = PROFILE_DMA_LEAD;
  }
  for (uint32_t i = 0; i < n; i++) {
    uint32_t at = (f + i) & MASK;
    ring[at] = (int8_t)cart_buffer()[at ^ 1u];
  }
  st_front = e;
}

/* The chip plays what the ring holds, up to `until_q16`. */
static void chip_play(uint32_t until_q16) {
  while ((chip_q16 >> 16) < (until_q16 >> 16)) {
    if (n_played < sizeof played) played[n_played++] = ring[(chip_q16 >> 16) & MASK];
    chip_q16 += 1u << 16;
  }
}

/* One VBL: the ST's copy, then 20 ms with the writer every 4 ms, the chip
 * playing, and the main loop's top-up once unless stalled. */
static void frame(bool main_loop, bool st_skip) {
  st_vbl_dma(st_skip);
  uint32_t start = chip_q16;
  for (int i = 1; i <= WRITER_RUNS_PER_VBL; i++) {
    now_us += VBL_US / WRITER_RUNS_PER_VBL;
    chip_play(start + (uint32_t)((uint64_t)CHIP_PER_VBL_Q16 * (uint32_t)i /
                                 WRITER_RUNS_PER_VBL));
    writer->callback(writer);
    if (i == 2 && main_loop) audio_render_frame();
  }
}

/* --- The source ----------------------------------------------------------------- */

#define PATTERN 3001u
static int8_t pattern[PATTERN];

static void make_pattern(void) {
  for (uint32_t i = 0; i < PATTERN; i++) {
    int8_t v = (int8_t)((int32_t)((i * 37u + i / 91u) % 241u) - 120);
    pattern[i] = v != 0 ? v : 1; /* never silence: the start is findable */
  }
}

/* Where the source starts in what was played: the first non-silent sample. */
static uint32_t first_sound(uint32_t from) {
  for (uint32_t i = from; i < n_played; i++) {
    if (played[i] != 0) return i;
  }
  return n_played;
}

/* Played from `at` on: the pattern from `k` in order, where a sample may
 * repeat the one before it only when `holds` (an underrun). Returns how many
 * samples followed the rule. */
static uint32_t follows(uint32_t at, uint32_t k, bool holds, uint32_t *repeats) {
  uint32_t i = at;
  *repeats = 0;
  for (; i < n_played; i++) {
    if (played[i] == pattern[k % PATTERN]) {
      k++;
    } else if (holds && i > at && played[i] == played[i - 1]) {
      (*repeats)++;
    } else {
      break;
    }
  }
  return i - at;
}

/* A new session with `source` installed: no source across the boot (a
 * source keeps what it put in the FIFO), then the main loop's top-up, as
 * an app's loop runs long before the ST first reports. */
static void boot_with(uint8_t m, void (*source)(void)) {
  audio_set_fill_callback(NULL);
  n_played = 0;
  st_boot(m);
  source();
  audio_render_frame();
}

static void pattern_source(void) { audio_play_pcm_loop(pattern, PATTERN, AUDIO_DMA_RATE_HZ); }

static void start(uint8_t m) { boot_with(m, pattern_source); }

/* --- The cases -------------------------------------------------------------------- */

static void test_steady(void) {
  start(0x11);
  CHECK_EQ(audioOutput, 2);
  CHECK(audio_uses_dma());
  uint32_t late0 = audioLateSlices, under0 = audioUnderruns;
  for (int i = 0; i < 1000; i++) frame(true, false);
  uint32_t s = first_sound(0);
  CHECK(s < FIVE_VBLS); /* the sound starts within five VBLs */
  uint32_t repeats;
  CHECK_EQ(follows(s, 0, false, &repeats), n_played - s);
  CHECK_EQ(audioLateSlices, late0);
  CHECK_EQ(audioUnderruns, under0);
}

/* The main loop away for three VBLs every so often: the FIFO and the
 * samples written ahead cover it. */
static void test_stalls_covered(void) {
  start(0x11);
  uint32_t under0 = audioUnderruns;
  for (int i = 0; i < 1000; i++) frame(i % 25 >= 3 || i < 25, false);
  uint32_t s = first_sound(0), repeats;
  CHECK_EQ(follows(s, 0, false, &repeats), n_played - s);
  CHECK_EQ(audioUnderruns, under0);
}

/* Away for ten VBLs: the last sample is held, counted, and the source goes
 * on where it was. */
static void test_long_stall(void) {
  start(0x11);
  uint32_t under0 = audioUnderruns;
  for (int i = 0; i < 400; i++) frame(i < 100 || i >= 110, false);
  uint32_t s = first_sound(0), repeats;
  CHECK_EQ(follows(s, 0, true, &repeats), n_played - s);
  CHECK(repeats > 0u);
  CHECK(audioUnderruns > under0);
}

/* A blit that overran: the ST misses one copy; three VBLs of lead cover it. */
static void test_missed_copy(void) {
  start(0x11);
  uint32_t late0 = audioLateSlices;
  for (int i = 0; i < 600; i++) frame(true, i % 50 == 25);
  uint32_t s = first_sound(0), repeats;
  CHECK_EQ(follows(s, 0, false, &repeats), n_played - s);
  CHECK_EQ(audioLateSlices, late0);
}

/* The ST resets: its reports stop, then a hello and a new ring. The sound
 * comes back, in order, with nothing counted late. */
static void test_st_reset(void) {
  start(0x11);
  for (int i = 0; i < 200; i++) frame(true, false);
  uint32_t late0 = audioLateSlices;
  /* 300 ms of reboot: no reports, the chip silent. */
  for (int i = 0; i < 15; i++) {
    now_us += VBL_US;
    for (int j = 0; j < WRITER_RUNS_PER_VBL; j++) writer->callback(writer);
    audio_render_frame();
  }
  uint32_t before = n_played;
  st_boot(0x11);
  for (int i = 0; i < 300; i++) frame(true, false);
  uint32_t s = first_sound(before);
  CHECK(s - before < FIVE_VBLS);
  /* The source went on while the ST rebooted: find where. */
  uint32_t k = 0;
  while (k < PATTERN && played[s] != pattern[k]) k++;
  uint32_t best = 0, repeats;
  for (uint32_t kk = k; kk < PATTERN; kk++) {
    if (played[s] != pattern[kk]) continue;
    uint32_t n = follows(s, kk, false, &repeats);
    if (n > best) best = n;
  }
  CHECK_EQ(best, n_played - s);
  CHECK_EQ(audioLateSlices, late0);
}

/* YM pairs on the DMA chip: a pair of the table plays as the sample its
 * index stands for; a rising run of pairs plays rising. */
static uint8_t ym_const[224];
static uint8_t ym_ramp[2 * 64 * 20];
static unsigned n_ramp;

static void ym_ramp_source(void) { audio_play_loop(ym_ramp, n_ramp); }

static void ym_const_source(void) { audio_play_loop(ym_const, sizeof ym_const); }

static void test_ym_source_on_dma(void) {
  /* (4, 9) is entry 21 only: 21 * 4 + 2 - 128 = -42. */
  for (unsigned i = 0; i < sizeof ym_const; i += 2) {
    ym_const[i] = 4;
    ym_const[i + 1] = 9;
  }
  boot_with(0x11, ym_const_source);
  for (int i = 0; i < 100; i++) frame(true, false);
  uint32_t s = first_sound(0);
  bool all = s < n_played;
  for (uint32_t i = s + 16u; i < n_played; i++) all = all && played[i] == -42;
  CHECK(all);

  /* Entries 0..54 hold distinct, rising pairs; each for 20 samples. */
  static const uint8_t k_rising[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                     14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
  static const uint8_t k_pairs[][2] = {
      {0, 0}, {0, 2}, {1, 2}, {2, 2}, {2, 3}, {1, 4}, {2, 4}, {2, 5}, {0, 6},
      {2, 6}, {3, 6}, {4, 6}, {2, 7}, {4, 7}, {5, 7}, {2, 8}, {3, 8}, {4, 8},
      {5, 8}, {2, 9}, {3, 9}, {4, 9}, {5, 9}, {6, 9}, {7, 9}};
  unsigned n = 0;
  for (unsigned r = 0; r < sizeof k_rising; r++) {
    for (unsigned j = 0; j < 20; j++) {
      ym_ramp[n++] = k_pairs[k_rising[r]][0];
      ym_ramp[n++] = k_pairs[k_rising[r]][1];
    }
  }
  n_ramp = n;
  boot_with(0x11, ym_ramp_source);
  for (int i = 0; i < 100; i++) frame(true, false);
  /* Somewhere a full rise plays: from the end of the -126 step up to -30,
   * never falling on the way. */
  bool rose = false;
  for (uint32_t i = 0; i + 1u < n_played && !rose; i++) {
    if (played[i] != -126 || played[i + 1u] == -126) continue;
    uint32_t j = i;
    while (j + 1u < n_played && played[j + 1u] >= played[j]) j++;
    rose = played[j] == 24 * 4 + 2 - 128;
  }
  CHECK(rose);
}

/* PCM on the YM: the slices hold the table's pairs for the samples' upper
 * 6 bits, in order (the YM's own rate: no resampling). */
static void pattern_ym_rate_source(void) {
  audio_play_pcm_loop(pattern, PATTERN, PROFILE_YM_RATE_HZ);
}

static void test_pcm_source_on_ym(void) {
  boot_with(0x00, pattern_ym_rate_source);
  CHECK_EQ(audioOutput, 1);
  CHECK(!audio_uses_dma());
  const uint8_t *buf = cart_buffer();
  for (int i = 0; i < 40; i++) {
    st_vbl++;
    slice_report = (int)(st_vbl % CART_AUDIO_SLICES);
    for (int j = 0; j < WRITER_RUNS_PER_VBL; j++) {
      now_us += VBL_US / WRITER_RUNS_PER_VBL;
      writer->callback(writer);
      if (j == 2) audio_render_frame();
    }
  }
  /* The slice written ahead: 112 pairs for 112 samples of the pattern in a
   * row. Find where it starts. */
  const uint8_t *slice = buf + ((st_vbl + 1u) % CART_AUDIO_SLICES) * CART_AUDIO_SLICE_BYTES;
  static const uint8_t k_lut_a[64] = {0, 0, 1, 2, 2, 1, 2, 2, 0, 2, 3, 4, 2, 4, 5, 2,
                                      3, 4, 5, 2, 3, 4, 5, 6, 7, 3, 4, 5, 6, 7, 0, 1,
                                      2, 4, 5, 6, 7, 8, 8, 9, 9, 0, 1, 2, 3, 4, 5, 6,
                                      8, 8, 9, 9, 9, 10, 0, 2, 3, 4, 5, 6, 7, 8, 8, 9};
  static const uint8_t k_lut_b[64] = {0,  2,  2,  2,  3,  4,  4,  5,  6,  6,  6,  6,  7,
                                      7,  7,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  10,
                                      10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11,
                                      11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
                                      12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13};
  bool found = false;
  for (uint32_t k = 0; k < PATTERN && !found; k++) {
    bool ok = true;
    for (uint32_t i = 0; i < 112u && ok; i++) {
      uint32_t index = (uint32_t)(pattern[(k + i) % PATTERN] + 128) >> 2;
      ok = slice[2u * i] == k_lut_a[index] && slice[2u * i + 1u] == k_lut_b[index];
    }
    found = ok;
  }
  CHECK(found);
}

/* The app keeps to the YM: the word the ST reads says so, and the next
 * hello on an STE picks the YM; back to the DMA chip after. */
static void test_prefer_ym(void) {
  volatile uint16_t *out = (volatile uint16_t *)((uint8_t *)__rom_in_ram_start__ +
                                                 CART_AUDIO_OUT_OFFSET);
  audio_prefer_ym(true);
  CHECK_EQ(*out, CART_AUDIO_OUT_YM);
  st_boot(0x10);
  CHECK_EQ(audioOutput, 1);
  audio_prefer_ym(false);
  CHECK_EQ(*out, CART_AUDIO_OUT_AUTO);
  CHECK_EQ(audioOutput, 1); /* until the ST boots again */
  st_boot(0x10);
  CHECK_EQ(audioOutput, 2);
  st_boot(0x20);            /* a TT: no DMA path here */
  CHECK_EQ(audioOutput, 1);
}

/* A PCM source at another rate: half the DMA rate plays every sample twice,
 * give or take the interpolation. */
static int8_t flat[500];

static void flat_source(void) { audio_play_pcm_loop(flat, sizeof flat, 6258); }

static void test_resampling(void) {
  memset(flat, 55, sizeof flat);
  boot_with(0x11, flat_source);
  for (int i = 0; i < 60; i++) frame(true, false);
  uint32_t s = first_sound(0);
  bool all = s < n_played;
  for (uint32_t i = s; i < n_played; i++) all = all && played[i] == 55;
  CHECK(all);
}

int main(void) {
  make_pattern();
  audio_init();
  test_steady();
  test_stalls_covered();
  test_long_stall();
  test_missed_copy();
  test_st_reset();
  test_ym_source_on_dma();
  test_pcm_source_on_ym();
  test_prefer_ym();
  test_resampling();
  TEST_END();
}
