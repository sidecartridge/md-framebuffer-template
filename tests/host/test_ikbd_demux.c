// units: rp/src/ikbd_demux.c
/* The IKBD decoder (ikbd_demux.c): every packet type and every rule for
 * lost bytes, then a simulated IKBD and ST feeding it hours of keys, mouse
 * bursts and joystick events with damage injected: bytes the ACIA lost
 * (reported), bytes the RP's ring dropped (reported), bytes lost with no
 * report, bytes read twice.
 *
 * A key press the decoder emits must come from a key-press byte of that
 * key, never from packet data: with reported losses not once; with the
 * others the stream must still come back in step. Once the damage stops
 * and the stream has had a quiet frame, everything decoded must match what
 * the IKBD sent. */
#include <stdlib.h>
#include <string.h>

#include "ikbd_demux.h"
#include "test.h"

/* --- Key events, and where the byte being decoded came from ------------- */

typedef enum { FROM_NONE, FROM_PRESS, FROM_RELEASE, FROM_PACKET } origin_t;

typedef struct {
  uint8_t scancode;
  bool is_press;
} key_event_t;

static key_event_t events[64];
static int event_count;
static origin_t cur_origin;
static uint8_t cur_scancode;
static long phantom_presses;
static long real_presses_decoded;

static void on_key(void *ctx, uint8_t scancode, bool is_press) {
  (void)ctx;
  if (event_count < (int)(sizeof(events) / sizeof(events[0]))) {
    events[event_count].scancode = scancode;
    events[event_count].is_press = is_press;
    event_count++;
  }
  if (is_press) {
    if (cur_origin == FROM_PRESS && cur_scancode == scancode) {
      real_presses_decoded++;
    } else {
      phantom_presses++;
    }
  }
}

static ikbd_demux_t d;

static void fresh(void) {
  ikbd_demux_init(&d, on_key, NULL);
  event_count = 0;
  cur_origin = FROM_NONE;
  phantom_presses = 0;
  real_presses_decoded = 0;
}

static void feed(const uint8_t *bytes, int n) {
  for (int i = 0; i < n; i++) ikbd_demux_byte(&d, bytes[i]);
}

#define FEED(...)                                            \
  do {                                                       \
    const uint8_t feed_bytes_[] = {__VA_ARGS__};             \
    feed(feed_bytes_, (int)sizeof(feed_bytes_));             \
  } while (0)

/* --- One case per rule --------------------------------------------------- */

static void test_keys(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  FEED(0x1E, 0x9E);                /* A down, A up */
  FEED(0x00, 0x80);                /* not keys */
  FEED(0x70, 0x71, 0x72);          /* keypad 0 / . / Enter down */
  FEED(0xF0, 0xF1, 0xF2);          /* and up */
  CHECK_EQ(event_count, 8);
  CHECK_EQ(events[0].scancode, 0x1E);
  CHECK(events[0].is_press);
  CHECK_EQ(events[1].scancode, 0x1E);
  CHECK(!events[1].is_press);
  CHECK_EQ(events[5].scancode, 0x70);
  CHECK(!events[5].is_press);
  CHECK_EQ(events[7].scancode, 0x72);
  CHECK_EQ(d.power_ups, 0);
  ikbd_demux_tick(&d, 10);
  CHECK_EQ(ikbd_demux_resyncs(&d), 0);
  CHECK_EQ(d.bytes, 10);
}

static void test_mouse(void) {
  fresh();
  FEED(0xF8, 0x05, 0xFD);          /* dx +5, dy -3 */
  CHECK_EQ(d.mouse_dx, 5);
  CHECK_EQ(d.mouse_dy, -3);
  CHECK_EQ(d.mouse_buttons, 0);
  FEED(0xFA, 0x00, 0x00);          /* left down */
  CHECK_EQ(d.mouse_buttons, IKBD_MOUSE_LEFT);
  CHECK_EQ(d.mouse_pressed, IKBD_MOUSE_LEFT);
  FEED(0xF9, 0x01, 0x01);          /* left up, right down */
  CHECK_EQ(d.mouse_buttons, IKBD_MOUSE_RIGHT);
  CHECK_EQ(d.mouse_pressed, IKBD_MOUSE_LEFT | IKBD_MOUSE_RIGHT);
  CHECK_EQ(d.mouse_dx, 6);
  CHECK_EQ(d.mouse_dy, -2);
  FEED(0xF8, 0x80, 0x7F);          /* the extremes */
  CHECK_EQ(d.mouse_dx, 6 - 128);
  CHECK_EQ(d.mouse_dy, -2 + 127);
  CHECK_EQ(d.mouse_packets, 4);
  CHECK_EQ(event_count, 0);
}

static void test_keys_between_packets(void) {
  fresh();
  FEED(0xF8, 0x01, 0x02, 0x1E, 0xF8, 0x03, 0x04, 0x9E);
  CHECK_EQ(d.mouse_dx, 4);
  CHECK_EQ(d.mouse_dy, 6);
  CHECK_EQ(event_count, 2);
  CHECK_EQ(events[0].scancode, 0x1E);
  CHECK(events[0].is_press);
  CHECK(!events[1].is_press);
}

static void test_joysticks(void) {
  fresh();
  ikbd_demux_devices(&d, false, true, true);
  FEED(0xFE, 0x81);                /* stick 0: up + fire */
  CHECK_EQ(d.joy[0], IKBD_JOY_UP | IKBD_JOY_FIRE);
  CHECK_EQ(d.joy_pressed[0], IKBD_JOY_UP | IKBD_JOY_FIRE);
  FEED(0xFF, 0x08);                /* stick 1: right */
  CHECK_EQ(d.joy[1], IKBD_JOY_RIGHT);
  FEED(0xFE, 0x00);                /* stick 0 released: the latch stays */
  CHECK_EQ(d.joy[0], 0);
  CHECK_EQ(d.joy_pressed[0], IKBD_JOY_UP | IKBD_JOY_FIRE);
  FEED(0xFD, 0x02, 0x84);          /* both: down / left + fire */
  CHECK_EQ(d.joy[0], IKBD_JOY_DOWN);
  CHECK_EQ(d.joy[1], IKBD_JOY_LEFT | IKBD_JOY_FIRE);
  CHECK_EQ(d.joy_pressed[1], IKBD_JOY_RIGHT | IKBD_JOY_LEFT | IKBD_JOY_FIRE);
  CHECK_EQ(d.joy_events, 3);
  CHECK_EQ(d.joy_reports, 1);
  CHECK_EQ(event_count, 0);
}

static void test_mouse_and_joystick1(void) {
  fresh();
  ikbd_demux_devices(&d, true, false, true);
  FEED(0xF9, 0x00, 0x00);          /* right button = stick 1's fire */
  CHECK_EQ(d.joy[1], IKBD_JOY_FIRE);
  CHECK_EQ(d.joy_pressed[1], IKBD_JOY_FIRE);
  /* The stick's events carry no fire bit in this mode (a Mega ST's IKBD):
   * moving the stick while fire is held keeps the fire. */
  FEED(0xFF, 0x01);                /* stick 1 up, fire still held */
  CHECK_EQ(d.joy[1], IKBD_JOY_UP | IKBD_JOY_FIRE);
  FEED(0xF8, 0x00, 0x00);          /* fire released */
  CHECK_EQ(d.joy[1], IKBD_JOY_UP);
  FEED(0xFF, 0x00);                /* stick 1 centred */
  CHECK_EQ(d.joy[1], 0);
  FEED(0xFD, 0x0F, 0x02);          /* stick 0's byte is the mouse's wires */
  CHECK_EQ(d.joy[0], 0);
  CHECK_EQ(d.joy[1], IKBD_JOY_DOWN);
  ikbd_demux_devices(&d, false, false, false);
  CHECK_EQ(d.joy[1], 0);
  CHECK_EQ(d.mouse_buttons, 0);
  CHECK(!d.right_is_joy1_fire);
}

static void test_devices_released(void) {
  fresh();
  ikbd_demux_devices(&d, false, true, true);
  FEED(0xFD, 0x01, 0x02, 0xFA, 0x00, 0x00);
  ikbd_demux_devices(&d, true, false, false);
  CHECK_EQ(d.joy[0], 0);
  CHECK_EQ(d.joy[1], 0);
  CHECK_EQ(d.mouse_buttons, IKBD_MOUSE_LEFT);
  CHECK(!d.right_is_joy1_fire);
  ikbd_demux_devices(&d, false, true, true);
  CHECK_EQ(d.mouse_buttons, 0);
}

/* An overrun is reported just before the byte read with it; the byte after
 * that one is lost. */
static void test_overrun(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  ikbd_demux_loss(&d, IKBD_RESYNC_OVERRUN);
  FEED(0xF8, 0x01, /* 0x02 lost */ 0xF8, 0x05, 0x05, 0x1E);
  CHECK_EQ(event_count, 0);
  CHECK_EQ(d.dropped_presses, 3);
  CHECK_EQ(d.mouse_dx, 0);
  CHECK(d.guard);
  ikbd_demux_tick(&d, 6);          /* bytes this frame: still guarded */
  CHECK(d.guard);
  ikbd_demux_tick(&d, 6);          /* a quiet frame: back in step */
  CHECK(!d.guard);
  FEED(0x1E, 0xF8, 0x02, 0x03);
  CHECK_EQ(event_count, 1);
  CHECK_EQ(d.mouse_dx, 2);
  CHECK_EQ(d.resyncs[IKBD_RESYNC_OVERRUN], 1);
  CHECK_EQ(d.resyncs[IKBD_RESYNC_COUNT], 0);
  CHECK_EQ(d.guards, 1);
}

static void test_releases_pass_the_guard(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  FEED(0x1E);
  ikbd_demux_loss(&d, IKBD_RESYNC_OVERRUN);
  FEED(0x9E);
  CHECK_EQ(event_count, 2);
  CHECK(!events[1].is_press);
}

static void test_count_mismatch(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  FEED(0x1E);
  ikbd_demux_tick(&d, 2);          /* the ST read one more */
  CHECK_EQ(d.resyncs[IKBD_RESYNC_COUNT], 1);
  CHECK(d.guard);
  ikbd_demux_tick(&d, 2);
  CHECK(!d.guard);
  CHECK_EQ(d.count, 2);
  FEED(0x9E);
  ikbd_demux_tick(&d, 3);
  CHECK_EQ(d.resyncs[IKBD_RESYNC_COUNT], 1);
}

static void test_count_wraps(void) {
  fresh();
  ikbd_demux_tick(&d, 250);
  for (int i = 0; i < 10; i++) FEED(0xF8, 0x00, 0x00);
  ikbd_demux_tick(&d, (uint8_t)(250 + 30));
  CHECK_EQ(ikbd_demux_resyncs(&d), 0);
}

static void test_timeout(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  FEED(0xF8, 0x01);                /* dy never comes */
  ikbd_demux_tick(&d, 2);
  CHECK_EQ(d.need, 1);
  ikbd_demux_tick(&d, 2);          /* open at its second tick */
  CHECK_EQ(d.resyncs[IKBD_RESYNC_TIMEOUT], 1);
  CHECK_EQ(d.need, 0);
  CHECK(!d.guard);                 /* that frame was quiet */
  FEED(0x1E);
  CHECK_EQ(event_count, 1);
}

static void test_bad_joystick_byte(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  FEED(0xFE, 0x10, 0x1E);
  CHECK_EQ(d.resyncs[IKBD_RESYNC_JOYSTICK], 1);
  CHECK_EQ(d.joy[0], 0);
  CHECK_EQ(event_count, 0);
  CHECK(d.guard);
}

static void test_unexpected_packet(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  FEED(0xF6, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x1E);
  CHECK_EQ(d.resyncs[IKBD_RESYNC_UNEXPECTED], 1);
  CHECK_EQ(d.other_packets, 1);
  CHECK_EQ(event_count, 0);        /* its bytes were not keys; A in guard */
  CHECK_EQ(d.dropped_presses, 1);
}

static void test_injected(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  ikbd_demux_injected(&d, 0x1E);
  ikbd_demux_injected(&d, 0x9E);
  ikbd_demux_tick(&d, 0);
  CHECK_EQ(event_count, 2);
  CHECK_EQ(ikbd_demux_resyncs(&d), 0);
}

/* The IKBD powers up again: $F0 / $F1 releases the keys down, unless it is
 * the release of keypad 0 / keypad '.', which are down. */
static void test_power_up(void) {
  fresh();
  FEED(0x1E, 0x2C, 0xFA, 0x00, 0x00);      /* A, Z, the left button */
  event_count = 0;
  FEED(0xF1);
  CHECK_EQ(d.power_ups, 1);
  CHECK_EQ(event_count, 2);                /* A and Z released, nothing else */
  CHECK(!events[0].is_press && !events[1].is_press);
  CHECK_EQ(events[0].scancode, 0x1E);
  CHECK_EQ(events[1].scancode, 0x2C);
  event_count = 0;
  FEED(0x71, 0xF1, 0x70, 0xF0);            /* keypad '.' and 0, pressed and released */
  CHECK_EQ(d.power_ups, 1);
  CHECK_EQ(event_count, 4);
  CHECK(!events[1].is_press);
  CHECK_EQ(events[1].scancode, 0x71);
  FEED(0xF0);                              /* keypad 0 is up: a power-up */
  CHECK_EQ(d.power_ups, 2);
  CHECK_EQ(event_count, 4);
}

/* An ST side that never reports its count: a loss drops the open packet,
 * and nothing waits for a tick that will not come. */
static void test_no_ticks(void) {
  fresh();
  FEED(0xF8, 0x01);
  ikbd_demux_loss(&d, IKBD_RESYNC_OVERRUN);
  CHECK(!d.guard);
  CHECK_EQ(d.need, 0);
  FEED(0x1E);
  CHECK_EQ(event_count, 1);
}

static void test_session(void) {
  fresh();
  ikbd_demux_tick(&d, 0);
  ikbd_demux_devices(&d, false, true, true);
  FEED(0xFE, 0x81, 0xF8);
  ikbd_demux_session(&d);
  CHECK_EQ(d.joy[0], 0);
  CHECK_EQ(d.need, 0);
  CHECK(!d.count_known);
  ikbd_demux_tick(&d, 0);          /* the ST's count starts again */
  CHECK_EQ(ikbd_demux_resyncs(&d), 0);
  CHECK_EQ(d.bytes, 3);
}

/* --- A simulated IKBD and ST ---------------------------------------------- */

#define SLOT_US 1280     /* one IKBD byte */
#define VBL_US 20000

typedef enum { DMG_NONE, DMG_OVERRUN, DMG_RING_DROP, DMG_SILENT_DROP, DMG_DOUBLE } damage_t;

typedef struct {
  uint8_t b;
  origin_t origin;
  uint8_t scancode;
} sim_byte_t;

typedef struct {
  uint32_t rng;
  damage_t damage;
  int damage_one_in;   /* bytes */
  bool damaging;
  bool generating;
  /* The IKBD. */
  sim_byte_t msg[8];
  int msg_len;
  int msg_pos;
  int burst;           /* mouse packets left in this movement */
  bool held[128];
  uint8_t buttons;
  uint8_t joy[2];
  /* What it sent, since the snapshot. */
  long dx, dy;
  long presses;
  /* The ST: its count, the byte its ACIA holds when an overrun is due. */
  uint8_t count;
  bool have_held;
  sim_byte_t held_byte;
  int bytes_this_vbl;
  int drop_run;        /* bytes the RP's ring still drops */
} sim_t;

static uint32_t rnd(sim_t *s, uint32_t n) {
  s->rng = s->rng * 1103515245u + 12345u;
  return (s->rng >> 8) % n;
}

static void add(sim_t *s, uint8_t b, origin_t origin, uint8_t scancode) {
  s->msg[s->msg_len].b = b;
  s->msg[s->msg_len].origin = origin;
  s->msg[s->msg_len].scancode = scancode;
  s->msg_len++;
}

/* The IKBD's next message, when its line is free. */
static void next_message(sim_t *s) {
  s->msg_len = 0;
  s->msg_pos = 0;
  if (!s->generating) return;
  if (s->burst == 0) {
    uint32_t r = rnd(s, 1000);
    if (r < 30) {
      s->burst = 1 + (int)rnd(s, 60);
    } else if (r < 50) {
      uint8_t sc = (uint8_t)(1 + rnd(s, 0x72));
      if (s->held[sc]) {
        s->held[sc] = false;
        add(s, (uint8_t)(sc | 0x80u), FROM_RELEASE, sc);
      } else {
        s->held[sc] = true;
        add(s, sc, FROM_PRESS, sc);
        s->presses++;
      }
      return;
    } else if (r < 60) {
      unsigned port = rnd(s, 2);
      uint8_t state = (uint8_t)(rnd(s, 16) | (rnd(s, 2) ? IKBD_JOY_FIRE : 0u));
      s->joy[port] = state;
      add(s, (uint8_t)(0xFE + port), FROM_PACKET, 0);
      add(s, state, FROM_PACKET, 0);
      return;
    } else if (r < 61) {
      add(s, 0xFD, FROM_PACKET, 0);
      add(s, s->joy[0], FROM_PACKET, 0);
      add(s, s->joy[1], FROM_PACKET, 0);
      return;
    } else {
      return;                      /* the line stays idle */
    }
  }
  s->burst--;
  if (rnd(s, 8) == 0) s->buttons = (uint8_t)rnd(s, 4);
  int dx = rnd(s, 10) == 0 ? (int)rnd(s, 256) - 128 : (int)rnd(s, 21) - 10;
  int dy = rnd(s, 10) == 0 ? (int)rnd(s, 256) - 128 : (int)rnd(s, 21) - 10;
  s->dx += dx;
  s->dy += dy;
  add(s, (uint8_t)(0xF8u | s->buttons), FROM_PACKET, 0);
  add(s, (uint8_t)dx, FROM_PACKET, 0);
  add(s, (uint8_t)dy, FROM_PACKET, 0);
}

static bool damage_now(sim_t *s, damage_t kind) {
  return s->damaging && s->damage == kind && rnd(s, (uint32_t)s->damage_one_in) == 0;
}

/* The RP: the ring between the ST's reads and the decoder. When it is full
 * it drops everything, bytes and ticks, until ikbd_pump drains it and
 * reports the loss: in the decoder's order, the report comes before the
 * first sample after the drops. */
static void rp_byte(sim_t *s, sim_byte_t b) {
  if (s->drop_run == 0) {
    if (damage_now(s, DMG_RING_DROP)) {
      s->drop_run = 1 + (int)rnd(s, 4);
      ikbd_demux_loss(&d, IKBD_RESYNC_DROPPED);
    } else if (damage_now(s, DMG_SILENT_DROP)) {
      s->drop_run = 1 + (int)rnd(s, 4);
    }
  }
  if (s->drop_run > 0) {
    s->drop_run--;
    return;
  }
  cur_origin = b.origin;
  cur_scancode = b.scancode;
  ikbd_demux_byte(&d, b.b);
  cur_origin = FROM_NONE;
}

/* The ST reads a byte, counts it and forwards it. */
static void st_read(sim_t *s, sim_byte_t b) {
  s->count++;
  rp_byte(s, b);
  if (damage_now(s, DMG_DOUBLE)) {
    s->count++;
    rp_byte(s, b);
  }
}

/* A byte reaches the ACIA. The ST reads each one a moment later, so an
 * overrun is modelled with one byte of delay: the byte waiting in the ACIA
 * is read (after the overrun report) and the new one is lost. */
static void st_receive(sim_t *s, sim_byte_t b) {
  s->bytes_this_vbl++;
  if (s->have_held && damage_now(s, DMG_OVERRUN)) {
    ikbd_demux_loss(&d, IKBD_RESYNC_OVERRUN);
    st_read(s, s->held_byte);
    s->have_held = false;
    return;
  }
  if (s->have_held) st_read(s, s->held_byte);
  s->held_byte = b;
  s->have_held = true;
}

static void st_vbl(sim_t *s) {
  if (s->have_held) {
    st_read(s, s->held_byte);
    s->have_held = false;
  }
  /* A full ring drops every sample, the ticks too, until it is drained. */
  if (s->drop_run == 0) ikbd_demux_tick(&d, s->count);
}

typedef struct {
  long phantoms;
  long guards;
  long count_resyncs;
  bool snapshot_taken;
  bool in_step;
} fuzz_result_t;

static fuzz_result_t fuzz(damage_t damage, uint32_t seed, int vbls) {
  sim_t s;
  memset(&s, 0, sizeof(s));
  s.rng = seed;
  s.damage = damage;
  s.damage_one_in = 300;
  s.damaging = damage != DMG_NONE;
  s.generating = true;
  fresh();
  ikbd_demux_devices(&d, false, true, true);

  const int damage_vbls = damage == DMG_NONE ? 0 : vbls * 6 / 10;
  const int stop_vbl = vbls - 5;
  bool snapshot = damage == DMG_NONE;
  uint32_t resyncs_at_snapshot = 0;
  long phantoms_at_snapshot = 0;
  long now = 0;
  long next_vbl = VBL_US;
  int vbl = 0;
  while (vbl < vbls) {
    while (next_vbl <= now) {
      st_vbl(&s);
      vbl++;
      next_vbl += VBL_US;
      if (vbl == damage_vbls) s.damaging = false;
      if (vbl == stop_vbl) s.generating = false;
      /* Once the damage is over: the first quiet frame the decoder has
       * no packet open at, with the IKBD between messages, is where it
       * must be back in step. Everything decoded after it must match. */
      if (!snapshot && vbl > damage_vbls && s.bytes_this_vbl == 0 && !d.guard &&
          d.need == 0 && s.msg_pos == s.msg_len && s.drop_run == 0) {
        snapshot = true;
        resyncs_at_snapshot = ikbd_demux_resyncs(&d);
        d.mouse_dx = 0;
        d.mouse_dy = 0;
        s.dx = 0;
        s.dy = 0;
        s.presses = 0;
        real_presses_decoded = 0;
        phantoms_at_snapshot = phantom_presses;
      }
      s.bytes_this_vbl = 0;
    }
    if (s.msg_pos == s.msg_len) next_message(&s);
    if (s.msg_pos < s.msg_len) st_receive(&s, s.msg[s.msg_pos++]);
    now += SLOT_US;
  }

  fuzz_result_t r;
  r.phantoms = phantom_presses;
  r.guards = (long)d.guards;
  r.count_resyncs = (long)d.resyncs[IKBD_RESYNC_COUNT];
  r.snapshot_taken = snapshot;
  r.in_step = snapshot && !d.guard && d.need == 0 &&
              ikbd_demux_resyncs(&d) == resyncs_at_snapshot &&
              d.mouse_dx == s.dx && d.mouse_dy == s.dy &&
              d.mouse_buttons == s.buttons && d.joy[0] == s.joy[0] &&
              d.joy[1] == s.joy[1] && real_presses_decoded == s.presses &&
              phantom_presses == phantoms_at_snapshot;
  if (!r.in_step) {
    fprintf(stderr,
            "fuzz damage %d seed %u: snapshot %d guard %d need %d resyncs "
            "%u/%u dx %ld/%ld dy %ld/%ld buttons %u/%u joy %u/%u %u/%u "
            "presses %ld/%ld\n",
            (int)damage, (unsigned)seed, snapshot, d.guard, d.need,
            (unsigned)ikbd_demux_resyncs(&d), (unsigned)resyncs_at_snapshot,
            (long)d.mouse_dx, s.dx, (long)d.mouse_dy, s.dy, d.mouse_buttons,
            s.buttons, d.joy[0], s.joy[0], d.joy[1], s.joy[1],
            real_presses_decoded, s.presses);
  }
  return r;
}

static void test_fuzz(void) {
  long guards[5] = {0};
  long phantoms[5] = {0};
  long count_resyncs[5] = {0};
  for (uint32_t seed = 1; seed <= 40; seed++) {
    for (int k = DMG_NONE; k <= DMG_DOUBLE; k++) {
      fuzz_result_t r = fuzz((damage_t)k, seed * 7919u + (uint32_t)k, 3000);
      CHECK(r.snapshot_taken);
      CHECK(r.in_step);
      guards[k] += r.guards;
      phantoms[k] += r.phantoms;
      count_resyncs[k] += r.count_resyncs;
      /* Reported losses never turn packet bytes into key presses. */
      if (k == DMG_NONE || k == DMG_OVERRUN || k == DMG_RING_DROP) {
        CHECK_EQ(r.phantoms, 0);
      }
    }
  }
  /* The damage happened and was seen. */
  CHECK_EQ(guards[DMG_NONE], 0);
  CHECK(guards[DMG_OVERRUN] > 0);
  CHECK(guards[DMG_RING_DROP] > 0);
  CHECK(count_resyncs[DMG_SILENT_DROP] > 0);
  /* The ST does not count a byte its ACIA lost: overruns are not count
   * mismatches. A byte the RP dropped is one. */
  CHECK_EQ(count_resyncs[DMG_OVERRUN], 0);
  CHECK(count_resyncs[DMG_RING_DROP] > 0);
  printf("fuzz: guards %ld/%ld/%ld/%ld, phantom presses with silent drops %ld, "
         "with double reads %ld\n",
         guards[DMG_OVERRUN], guards[DMG_RING_DROP], guards[DMG_SILENT_DROP],
         guards[DMG_DOUBLE], phantoms[DMG_SILENT_DROP], phantoms[DMG_DOUBLE]);
}

int main(void) {
  test_keys();
  test_mouse();
  test_keys_between_packets();
  test_joysticks();
  test_mouse_and_joystick1();
  test_devices_released();
  test_overrun();
  test_releases_pass_the_guard();
  test_count_mismatch();
  test_count_wraps();
  test_timeout();
  test_bad_joystick_byte();
  test_unexpected_packet();
  test_injected();
  test_no_ticks();
  test_power_up();
  test_session();
  test_fuzz();
  TEST_END();
}
