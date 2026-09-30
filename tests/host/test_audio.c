// units: rp/src/audio.c
/* The audio producer (audio.c) against a simulated ST: at every VBL the ST
 * moves to the next slice and reports it, the slice writer's timer runs five
 * times per VBL, and the main loop tops up the FIFO once per frame unless
 * the test stalls it.
 *
 * Every slice the ST plays must be the next piece of the stream, or, when
 * the FIFO ran dry, the last sample held; a slice is never rewritten while
 * it plays; late slices and underruns are counted as the RP sees them. And
 * the .YMS reader: header checks, the wrap at the end of the file, a failed
 * read. */
#include <string.h>

#include "audio.h"
#include "cart_shared.h"
#include "ff.h"
#include "pico/time.h"
#include "test.h"

#define VBL_US 20000u
#define WRITER_RUNS_PER_VBL 5
#define CHUNK 224u /* AUDIO_FILL_BYTES_PER_VBL */

unsigned int __rom_in_ram_start__[0x10000 / sizeof(unsigned int)];
extern uint32_t audioSlicesWritten, audioUnderruns, audioLateSlices;

/* --- The RP's clock, timer and ROM3 ring --------------------------------- */

static uint32_t now_us;
static repeating_timer_t *writer;
static int st_report = -1; /* the slice of the latest report, -1: none */

uint32_t time_us_32(void) { return now_us; }

/* A plain ST said hello: the YM output. */
uint32_t st_session_hellos(void) { return 0; }
uint8_t st_session_machine(void) { return 0; }

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
  if (st_report < 0 || (match & mask) != CART_ROM3_AUDIO_SLICE_WINDOW) {
    return false;
  }
  *sample = (uint16_t)(CART_ROM3_AUDIO_SLICE_WINDOW | (uint16_t)st_report);
  return true;
}

/* --- Files in memory ------------------------------------------------------ */

static const BYTE *file_data;
static FSIZE_t file_size;
static const char *file_name;
static int fail_next_reads; /* the next reads fail, as a pulled card does */

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)mode;
  if (!file_name || strcmp(path, file_name) != 0) return FR_NO_FILE;
  fp->data = file_data;
  fp->size = file_size;
  fp->pos = 0;
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  if (fail_next_reads > 0) {
    fail_next_reads--;
    *br = 0;
    return FR_DISK_ERR;
  }
  UINT n = fp->pos + btr <= fp->size ? btr : (UINT)(fp->size - fp->pos);
  memcpy(buff, fp->data + fp->pos, n);
  fp->pos += n;
  *br = n;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  fp->pos = ofs;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  (void)fp;
  return FR_OK;
}

/* --- The ST ---------------------------------------------------------------- */

static uint8_t *slice(uint32_t s) {
  return (uint8_t *)__rom_in_ram_start__ + CART_AUDIO_BUFFER_OFFSET +
         (s % CART_AUDIO_SLICES) * CART_AUDIO_SLICE_BYTES;
}

static uint32_t st_vbl;   /* the ST's VBLs */
static uint8_t playing[CART_AUDIO_SLICE_BYTES];
static int torn;          /* a slice changed while it played */

/* One frame: the VBL (the ST moves to the next slice and reports it), then
 * 20 ms with the writer running every 4 ms, and the main loop's top-up
 * unless stalled. Returns the slice the ST played this frame. */
static const uint8_t *frame(bool main_loop, bool writer_runs) {
  st_vbl++;
  st_report = (int)(st_vbl % CART_AUDIO_SLICES);
  memcpy(playing, slice(st_vbl), sizeof playing);
  for (int i = 0; i < WRITER_RUNS_PER_VBL; i++) {
    now_us += VBL_US / WRITER_RUNS_PER_VBL;
    if (writer_runs) writer->callback(writer);
    if (i == 2 && main_loop) audio_render_frame();
  }
  if (memcmp(playing, slice(st_vbl), sizeof playing) != 0) torn++;
  return playing;
}

/* --- The stream ------------------------------------------------------------ */

static uint8_t stream[CHUNK * 7 + 100]; /* not a whole number of slices */

static void make_stream(void) {
  for (unsigned i = 0; i < sizeof stream; i++) {
    stream[i] = (uint8_t)(i * 7u + i / 251u + 1u);
  }
}

/* Does `s` hold chunk n of the looped stream? */
static bool is_chunk(const uint8_t *s, uint32_t n) {
  for (uint32_t j = 0; j < CHUNK; j++) {
    if (s[j] != stream[(n * CHUNK + j) % sizeof stream]) return false;
  }
  return true;
}

/* The chunk of the looped stream that `s` holds, looking from `from` up to
 * `span` chunks on; -1 if none. After a disruption the stream goes on, but
 * the test does not work out exactly where. */
static long find_chunk(const uint8_t *s, uint32_t from, uint32_t span) {
  for (uint32_t n = from; n < from + span; n++) {
    if (is_chunk(s, n)) return (long)n;
  }
  return -1;
}

/* Does `s` hold one sample pair over the whole slice (an underrun)? */
static bool is_hold(const uint8_t *s, uint8_t a, uint8_t b) {
  for (uint32_t j = 0; j < CART_AUDIO_SLICE_BYTES; j += 2) {
    if (s[j] != a || s[j + 1] != b) return false;
  }
  return true;
}

int main(void) {
  make_stream();
  audio_init();
  CHECK(writer != NULL);
  CHECK_EQ(writer->delay_us < 0 ? -writer->delay_us : writer->delay_us,
           VBL_US / WRITER_RUNS_PER_VBL);
  audio_play_loop(stream, sizeof stream);
  audio_render_frame(); /* the FIFO fills before the ST starts */

  /* The ST starts: the first report tells the writer where it is; the slice
   * after it is the first of the stream. */
  frame(true, true);
  uint32_t next = 0;
  for (int f = 0; f < 200; f++) {
    const uint8_t *s = frame(true, true);
    CHECK(is_chunk(s, next));
    next++;
  }
  CHECK_EQ(audioLateSlices, 0);
  CHECK_EQ(audioUnderruns, 0);
  CHECK_EQ(torn, 0);

  /* The main loop misses 3 frames: 80 ms between two of its top-ups, what
   * the FIFO's 4 slices cover. Nothing is held; the stream goes on. */
  for (int f = 0; f < 3; f++) {
    CHECK(is_chunk(frame(false, true), next));
    next++;
  }
  for (int f = 0; f < 10; f++) {
    CHECK(is_chunk(frame(true, true), next));
    next++;
  }
  CHECK_EQ(audioUnderruns, 0);

  /* It misses 8: the FIFO runs dry, and the slices written then hold the
   * last sample; the stream resumes where it stopped. */
  uint32_t held = 0;
  for (int f = 0; f < 8; f++) {
    const uint8_t *s = frame(false, true);
    if (is_chunk(s, next)) {
      next++;
    } else {
      const uint8_t *last = &stream[(next * CHUNK - 2u) % sizeof stream];
      CHECK(is_hold(s, last[0], last[1]));
      held++;
    }
  }
  for (int f = 0; f < 10; f++) {
    const uint8_t *s = frame(true, true);
    if (is_chunk(s, next)) {
      next++;
    } else {
      const uint8_t *last = &stream[(next * CHUNK - 2u) % sizeof stream];
      CHECK(is_hold(s, last[0], last[1]));
      held++;
    }
  }
  CHECK(held > 0);
  CHECK_EQ(audioUnderruns, held);
  CHECK_EQ(audioLateSlices, 0);
  uint32_t underruns = audioUnderruns;

  /* The writer misses 2 frames (interrupts off for a flash write, about
   * 50 ms): the ST reaches a slice that was not written for it, counted as
   * late. Then the stream goes on: no chunk lost or repeated. */
  for (int f = 0; f < 2; f++) frame(true, false);
  frame(true, true);
  CHECK_EQ(audioLateSlices, 1);
  long found = find_chunk(frame(true, true), next, 4);
  CHECK(found >= 0);
  next = (uint32_t)found + 1;
  for (int f = 0; f < 10; f++) {
    CHECK(is_chunk(frame(true, true), next));
    next++;
  }
  CHECK_EQ(audioLateSlices, 1);

  /* The ST leaves (GEM): no new report for longer than the timeout. Then it
   * boots again and starts over at slice 1. Its first slice is what was left
   * there; the two slices written ahead before it left are never played; no
   * late slice, no underrun, and the stream goes on from there. */
  for (int i = 0; i < 60; i++) {
    now_us += 4000u;
    writer->callback(writer);
  }
  audio_render_frame();
  st_vbl = 0;
  uint32_t late = audioLateSlices;
  frame(true, true);
  CHECK(is_chunk(frame(true, true), next + 2));
  next += 3;
  for (int f = 0; f < 20; f++) {
    CHECK(is_chunk(frame(true, true), next));
    next++;
  }
  CHECK_EQ(audioLateSlices, late);
  CHECK_EQ(audioUnderruns, underruns);
  CHECK_EQ(torn, 0);

  /* .YMS files: the header is checked, and a failure keeps the loop. */
  static BYTE yms[16 + CHUNK * 2 + 50]; /* the body ends inside a chunk */
  memcpy(yms, "YMS1", 4);
  uint32_t rate = 5585, len = sizeof yms - 16;
  memcpy(yms + 4, &rate, 4); /* the host is little-endian, as the file */
  memcpy(yms + 8, &len, 4);
  yms[12] = 1; /* dual-ghost */
  for (unsigned i = 16; i < sizeof yms; i++) yms[i] = (BYTE)(i * 3u + 5u);
  file_name = "A.YMS";
  file_data = yms;
  file_size = sizeof yms;
  CHECK_EQ(audio_play_yms_file("B.YMS"), -1); /* no such file */
  yms[0] = 'X';
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* bad magic */
  yms[0] = 'Y';
  yms[4] = 0xB0; /* 5,552 Hz */
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* another rate */
  memcpy(yms + 4, &rate, 4);
  yms[12] = 2;
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* another mode */
  yms[12] = 1;
  file_size = 10;
  CHECK_EQ(audio_play_yms_file("A.YMS"), -1); /* short header */
  file_size = sizeof yms;
  for (int f = 0; f < 10; f++) {
    CHECK(is_chunk(frame(true, true), next));
    next++;
  }

  /* Streaming: once the loop's chunks already queued have played, every
   * slice is the next piece of the file's body, which wraps at its end to
   * the body's start, never to the header. */
  CHECK_EQ(audio_play_yms_file("A.YMS"), 0);
  const BYTE *body = yms + 16;
  const uint32_t body_len = len;
  uint32_t ymsn = 0;
  bool started = false;
  for (int f = 0; f < 30; f++) {
    const uint8_t *s = frame(true, true);
    bool match = true;
    for (uint32_t j = 0; j < CHUNK; j++) {
      if (s[j] != body[(ymsn * CHUNK + j) % body_len]) match = false;
    }
    if (!started) {
      if (match) started = true;
      else continue;
    }
    CHECK(match);
    ymsn++;
  }
  CHECK(started);
  CHECK(ymsn >= 20); /* crossed the end of the file several times */

  /* A failed read (a pulled card) gives one slice of silence, and the next
   * read starts again at the body's start. */
  fail_next_reads = 1;
  bool silence = false;
  uint32_t after = 0;
  for (int f = 0; f < 12; f++) {
    const uint8_t *s = frame(true, true);
    if (!silence) {
      bool zero = true;
      for (uint32_t j = 0; j < CART_AUDIO_SLICE_BYTES; j++) {
        if (s[j] != 0) zero = false;
      }
      silence = zero;
      continue;
    }
    bool match = true;
    for (uint32_t j = 0; j < CHUNK; j++) {
      if (s[j] != body[(after * CHUNK + j) % body_len]) match = false;
    }
    CHECK(match);
    after++;
  }
  CHECK(silence);
  CHECK(after > 0);
  CHECK_EQ(audioLateSlices, late);
  CHECK_EQ(torn, 0);

  TEST_END();
}
