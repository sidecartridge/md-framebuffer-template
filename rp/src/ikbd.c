/**
 * File: ikbd.c
 * Description: The ST's keyboard, mouse and joysticks: ingest, input mode,
 *              and what the app reads (see ikbd.h).
 *
 * ikbd_consume_rom3_sample (commemul_poll's callback, from fb.c) keeps the
 * samples the IKBD path needs in a ring, in the order the ST produced them;
 * ikbd_pump drains it on the main loop into the decoder (ikbd_demux.c),
 * which calls push_key for every key and keeps the mouse and joystick
 * state. Everything runs on the main loop: `volatile` is for compiler
 * hygiene and for the counters read over SWD.
 */

#include "ikbd.h"

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "ikbd_demux.h"
#include "pico/stdlib.h"
#include "pico/time.h"

/* IKBD scancode for the ESC key. ESC press+release within
 * IKBD_ESC_RELEASE_TIMEOUT_US triggers CMD_BOOT_GEM via the cart
 * command sentinel. */
#define IKBD_SCANCODE_ESC 0x01u
#define IKBD_ESC_RELEASE_TIMEOUT_US 200000u  /* 200 ms */

#define IKBD_RING_MASK (IKBD_RING_CAPACITY - 1u)

#if (IKBD_RING_CAPACITY & IKBD_RING_MASK) != 0
#error "IKBD_RING_CAPACITY must be a power of two"
#endif

/* A byte a host tool typed (ikbd_inject_byte), in the ring with the ST's
 * samples: a window no ST read uses. */
#define IKBD_INJECTED_WINDOW 0x0100u

/* Sample ring (producer = ikbd_consume_rom3_sample and ikbd_inject_byte,
 * consumer = ikbd_pump): the ROM3 samples as captured, window in the high
 * byte. 256 entries: 16 frames of a mouse moved flat out. */
static volatile uint16_t s_ring[IKBD_RING_CAPACITY];
static volatile uint16_t s_head = 0;
static volatile uint16_t s_tail = 0;
static volatile uint32_t s_dropped = 0;
static uint32_t s_dropped_seen = 0;

static ikbd_demux_t s_demux;

/* Counters readable over SWD by symbol (tools/dev/swd.py counters), copied
 * from the decoder after each pump. */
volatile uint32_t ikbdOverruns = 0;       /* the ST's ACIA lost bytes */
volatile uint32_t ikbdBytes = 0;          /* IKBD bytes decoded */
volatile uint32_t ikbdMousePackets = 0;
volatile uint32_t ikbdJoystickPackets = 0; /* events and reports */
volatile uint32_t ikbdResyncs = 0;        /* every cause, overruns too */
volatile uint32_t ikbdCountMismatches = 0;
volatile uint32_t ikbdPowerUps = 0;       /* the IKBD restarted */

static uint32_t s_overruns_reported = 0;
static uint32_t s_resyncs_reported[IKBD_RESYNC_CAUSES];

/* Decoded key event ring. 16 entries; main-loop producer/consumer
 * (no IRQ-safety needed). */
#define IKBD_KEY_RING_SIZE 16u
#define IKBD_KEY_RING_MASK (IKBD_KEY_RING_SIZE - 1u)

static ikbd_key_event_t s_key_ring[IKBD_KEY_RING_SIZE];
static uint8_t          s_key_head = 0;
static uint8_t          s_key_tail = 0;

/* ESC press+release timestamp (microseconds, 0 = no press pending). */
static uint32_t s_esc_press_us = 0;

/* When true (default), ESC press+release pairs write CMD_BOOT_GEM to
 * the cart sentinel and userfw exits to GEM. Apps that want to own
 * the ESC key (e.g. menu+demo dispatcher) clear this via
 * ikbd_set_esc_auto_exit(false) -- ESC events are still delivered
 * through ikbd_pop_key, the auto-write is just gated off. */
static bool s_esc_auto_exit = true;

/* The mode the app asked for, the generation of its commands, and the mode
 * whose commands the ST last reported sent. */
static ikbd_input_mode_t s_mode = IKBD_INPUT_KEYBOARD;
static uint16_t s_mode_gen = 0;
static int s_live_mode = -1;

/* The last generation written to the IKBD command block. */
static uint16_t s_out_gen = 0;

/* The IKBD commands of each input mode, and what the mode reports. None
 * uses $1A (disable joysticks): on a Mega ST's IKBD, stick 1 stayed silent
 * after it, even once $14 had turned joystick events back on. $15 (joystick
 * interrogation mode: the sticks report only when asked) stops the events
 * instead. $14 turns the mouse off, so $08 comes after it; $16 asks for the
 * sticks' state once, since events only report changes.
 *
 * The mouse threshold ($0B x y, relative mode only, so after $08): the IKBD
 * sends a packet once the mouse has moved that many counts, not after every
 * count. Every byte costs the ST an interrupt of about 40 us; a mouse moved
 * fast at threshold 1 fills the line (16 bytes a VBL, about 0.6 ms) and a
 * full-screen blit then misses VBLs (a Mega ST: 50 -> 40 frames a second in
 * a demo). The movement adds up the same; a packet carries up to that many
 * counts. */
#define IKBD_MOUSE_THRESHOLD 4u

typedef struct {
  uint8_t len;
  uint8_t cmd[8];
  bool mouse, joy0, joy1;
} ikbd_mode_def_t;

static const ikbd_mode_def_t k_modes[IKBD_INPUT_MODES] = {
    [IKBD_INPUT_KEYBOARD] = {2, {0x12, 0x15}, false, false, false},
    [IKBD_INPUT_MOUSE] = {5,
                          {0x15, 0x08, 0x0B, IKBD_MOUSE_THRESHOLD,
                           IKBD_MOUSE_THRESHOLD},
                          true, false, false},
    [IKBD_INPUT_MOUSE_JOY1] = {6,
                               {0x14, 0x08, 0x0B, IKBD_MOUSE_THRESHOLD,
                                IKBD_MOUSE_THRESHOLD, 0x16},
                               true, false, true},
    [IKBD_INPUT_JOYSTICKS] = {3, {0x12, 0x14, 0x16}, false, true, true},
};

#if defined(_DEBUG) && (_DEBUG != 0)
/* Every sample the decoder saw, in order, for host tools to record the
 * stream (tools/dev/swd.py ikbd-log): a ring of IKBD_LOG_ENTRIES, and the
 * number of samples ever written to it. */
#define IKBD_LOG_ENTRIES 2048u
volatile uint16_t ikbdLog[IKBD_LOG_ENTRIES];
volatile uint32_t ikbdLogCount = 0;

static inline void log_sample(uint16_t sample) {
  ikbdLog[ikbdLogCount & (IKBD_LOG_ENTRIES - 1u)] = sample;
  ikbdLogCount++;
}
#else
#define log_sample(sample) ((void)0)
#endif

static inline void ring_push(uint16_t sample) {
  uint16_t next_head = (uint16_t)((s_head + 1u) & IKBD_RING_MASK);
  if (next_head != s_tail) {
    s_ring[s_head] = sample;
    s_head = next_head;
  } else {
    s_dropped++;
  }
}

void __not_in_flash_func(ikbd_consume_rom3_sample)(uint16_t addr_lsb) {
  switch (addr_lsb & CART_ROM3_WINDOW_MASK) {
    case IKBD_WINDOW_LO16:
    case CART_ROM3_IKBD_COUNT_WINDOW:
    case CART_ROM3_IKBD_OVERRUN_WINDOW:
    case CART_ROM3_IKBD_OUT_WINDOW:
    case CART_ROM3_HELLO_WINDOW:
      ring_push(addr_lsb);
      break;
    default:
      break;
  }
}

void ikbd_inject_byte(uint8_t b) { ring_push(IKBD_INJECTED_WINDOW | b); }

static void push_key(void *ctx, uint8_t scancode, bool is_press);

void ikbd_init(void) {
  s_head = 0;
  s_tail = 0;
  s_dropped = 0;
  s_dropped_seen = 0;
  s_key_head = 0;
  s_key_tail = 0;
  s_esc_press_us = 0;
  s_esc_auto_exit = true;
  ikbd_demux_init(&s_demux, push_key, NULL);
  for (unsigned i = 0; i < IKBD_RESYNC_CAUSES; i++) s_resyncs_reported[i] = 0;
  s_live_mode = -1;
  s_out_gen = 0;
  ikbd_set_input_mode(IKBD_INPUT_KEYBOARD);
}

void ikbd_set_esc_auto_exit(bool enabled) {
  s_esc_auto_exit = enabled;
}

size_t ikbd_ring_count(void) {
  uint16_t h = s_head;
  uint16_t t = s_tail;
  return (size_t)((h - t) & IKBD_RING_MASK);
}

uint32_t ikbd_ring_dropped(void) { return s_dropped; }

uint32_t ikbd_overruns(void) { return ikbdOverruns; }

const ikbd_demux_t *ikbd_demux(void) { return &s_demux; }

/* Pop one sample from the ring. Returns false if empty. */
static bool ring_pop(uint16_t *out) {
  uint16_t h = s_head;
  uint16_t t = s_tail;
  if (h == t) return false;
  *out = s_ring[t];
  s_tail = (uint16_t)((t + 1u) & IKBD_RING_MASK);
  return true;
}

/* The decoder's key callback. */
static void push_key(void *ctx, uint8_t scancode, bool is_press) {
  (void)ctx;
  /* ESC press+release pair → trigger exit via the cart command
   * sentinel. Pre-swap the longword halves so m68k's move.l sees
   * the expected value (cart_asM68kLong). */
  if (scancode == IKBD_SCANCODE_ESC) {
    if (is_press) {
      uint32_t now_us = time_us_32();
      if (now_us == 0u) now_us = 1u;  /* avoid the "no press" sentinel */
      s_esc_press_us = now_us;
    } else {
      uint32_t press = s_esc_press_us;
      s_esc_press_us = 0;
      if (s_esc_auto_exit && press != 0u &&
          (time_us_32() - press) < IKBD_ESC_RELEASE_TIMEOUT_US) {
        *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                                CART_CMD_SENTINEL_OFFSET)) =
            cart_asM68kLong(CART_CMD_BOOT_GEM);
      }
    }
  }

  uint8_t next = (uint8_t)((s_key_head + 1u) & IKBD_KEY_RING_MASK);
  if (next == s_key_tail) return;  /* ring full, drop event */
  s_key_ring[s_key_head].scancode = scancode;
  s_key_ring[s_key_head].is_press = is_press;
  s_key_head = next;
}

#if defined(_DEBUG) && (_DEBUG != 0)
static const char *const k_resync_names[IKBD_RESYNC_CAUSES] = {
    "ACIA overrun", "ring full", "byte count", "packet timeout",
    "bad joystick byte", "unexpected packet"};
#endif

void ikbd_pump(void) {
  uint16_t sample;
  while (ring_pop(&sample)) {
    log_sample(sample);
    uint8_t value = (uint8_t)(sample & 0xFFu);
    switch (sample & CART_ROM3_WINDOW_MASK) {
      case IKBD_WINDOW_LO16:
        ikbd_demux_byte(&s_demux, value);
        break;
      case IKBD_INJECTED_WINDOW:
        ikbd_demux_injected(&s_demux, value);
        break;
      case CART_ROM3_IKBD_COUNT_WINDOW:
        ikbd_demux_tick(&s_demux, value);
        break;
      case CART_ROM3_IKBD_OVERRUN_WINDOW:
        ikbdOverruns++;
        ikbd_demux_loss(&s_demux, IKBD_RESYNC_OVERRUN);
        break;
      case CART_ROM3_IKBD_OUT_WINDOW:
        DPRINTF("IKBD: commands #%u sent\n", (unsigned)value);
        if (value == (s_mode_gen & 0xFFu)) {
          const ikbd_mode_def_t *m = &k_modes[s_mode];
          s_live_mode = (int)s_mode;
          ikbd_demux_devices(&s_demux, m->mouse, m->joy0, m->joy1);
          DPRINTF("IKBD: input mode %u\n", (unsigned)s_mode);
        }
        break;
      case CART_ROM3_HELLO_WINDOW:
        /* The ST booted and reset the IKBD: nothing held any more, and the
         * mode's commands go out again (the block may hold others). */
        s_live_mode = -1;
        ikbd_demux_session(&s_demux);
        ikbd_set_input_mode(s_mode);
        break;
      default:
        break;
    }
  }
  /* The ring was full: what was lost came after everything drained here. */
  if (s_dropped != s_dropped_seen) {
    s_dropped_seen = s_dropped;
    ikbd_demux_loss(&s_demux, IKBD_RESYNC_DROPPED);
  }

  /* The IKBD restarted (a keyboard plugged back in) and is in its own mode
   * again: send the input mode again. */
  if (s_demux.power_ups != ikbdPowerUps) {
    ikbdPowerUps = s_demux.power_ups;
    DPRINTF("IKBD: it restarted; input mode %u sent again\n", (unsigned)s_mode);
    s_live_mode = -1;
    ikbd_set_input_mode(s_mode);
  }

  ikbdBytes = s_demux.bytes;
  ikbdMousePackets = s_demux.mouse_packets;
  ikbdJoystickPackets = s_demux.joy_events + s_demux.joy_reports;
  ikbdResyncs = ikbd_demux_resyncs(&s_demux);
  ikbdCountMismatches = s_demux.resyncs[IKBD_RESYNC_COUNT];

  if (ikbdOverruns != s_overruns_reported) {
    s_overruns_reported = ikbdOverruns;
    DPRINTF("IKBD: ACIA overrun #%lu, bytes were lost\n",
            (unsigned long)s_overruns_reported);
  }
#if defined(_DEBUG) && (_DEBUG != 0)
  for (unsigned i = 0; i < IKBD_RESYNC_CAUSES; i++) {
    if (s_demux.resyncs[i] != s_resyncs_reported[i]) {
      s_resyncs_reported[i] = s_demux.resyncs[i];
      DPRINTF("IKBD: resync (%s) #%lu\n", k_resync_names[i],
              (unsigned long)s_resyncs_reported[i]);
    }
  }
#endif
}

bool ikbd_pop_key(ikbd_key_event_t *out) {
  if (s_key_head == s_key_tail) return false;
  if (out) *out = s_key_ring[s_key_tail];
  s_key_tail = (uint8_t)((s_key_tail + 1u) & IKBD_KEY_RING_MASK);
  return true;
}

/* Put commands in the IKBD command block (cart_shared.h) under a new
 * generation: busy first, the bytes and the length, the generation last. */
static uint16_t write_commands(const uint8_t *cmd, size_t n) {
  volatile uint16_t *block =
      (volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                            CART_IKBD_OUT_OFFSET);
  s_out_gen = (uint16_t)((s_out_gen + 1u) & ~CART_IKBD_OUT_BUSY);
  block[0] = (uint16_t)(CART_IKBD_OUT_BUSY | s_out_gen);
  __sync_synchronize();
  for (size_t i = 0; i < CART_IKBD_OUT_MAX / 2u; i++) {
    uint8_t first = 2u * i < n ? cmd[2u * i] : 0u;
    uint8_t second = 2u * i + 1u < n ? cmd[2u * i + 1u] : 0u;
    block[2u + i] = (uint16_t)((first << 8) | second);
  }
  block[1] = (uint16_t)n;
  __sync_synchronize();
  block[0] = s_out_gen;
  return s_out_gen;
}

void ikbd_set_input_mode(ikbd_input_mode_t mode) {
  if ((unsigned)mode >= IKBD_INPUT_MODES) return;
  s_mode = mode;
  s_mode_gen = write_commands(k_modes[mode].cmd, k_modes[mode].len);
}

bool ikbd_send_commands(const uint8_t *cmd, size_t n) {
  if (n > CART_IKBD_OUT_MAX) return false;
  write_commands(cmd, n);
  return true;
}

ikbd_input_mode_t ikbd_get_input_mode(void) { return s_mode; }

int ikbd_get_live_input_mode(void) { return s_live_mode; }

static int16_t clamp16(int32_t v) {
  if (v > INT16_MAX) return INT16_MAX;
  if (v < INT16_MIN) return INT16_MIN;
  return (int16_t)v;
}

void ikbd_read_mouse(ikbd_mouse_t *out) {
  out->dx = clamp16(s_demux.mouse_dx);
  out->dy = clamp16(s_demux.mouse_dy);
  out->buttons = s_demux.mouse_buttons;
  out->pressed = s_demux.mouse_pressed;
  s_demux.mouse_dx = 0;
  s_demux.mouse_dy = 0;
  s_demux.mouse_pressed = 0;
}

void ikbd_read_joystick(unsigned port, ikbd_joystick_t *out) {
  if (port > 1u) {
    out->state = 0;
    out->pressed = 0;
    return;
  }
  out->state = s_demux.joy[port];
  out->pressed = s_demux.joy_pressed[port];
  s_demux.joy_pressed[port] = 0;
}
