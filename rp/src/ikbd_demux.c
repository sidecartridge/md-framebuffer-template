/**
 * File: ikbd_demux.c
 * Description: The IKBD byte stream -> keys, mouse and joysticks. See
 *              ikbd_demux.h for what it does when bytes are lost.
 */

#include "ikbd_demux.h"

#include <string.h>

/* Bytes after each packet header, $F6..$FF. */
static const uint8_t k_payload_bytes[10] = {7, 5, 2, 2, 2, 2, 6, 2, 1, 1};

/* The bits a joystick byte never has. */
#define JOY_UNUSED_BITS 0x70u

static bool key_down(const ikbd_demux_t *d, uint8_t scancode) {
  return (d->keys_down[scancode >> 3] >> (scancode & 7u)) & 1u;
}

static void emit_key(ikbd_demux_t *d, uint8_t scancode, bool is_press) {
  d->keys++;
  if (is_press) {
    d->keys_down[scancode >> 3] |= (uint8_t)(1u << (scancode & 7u));
  } else {
    d->keys_down[scancode >> 3] &= (uint8_t)~(1u << (scancode & 7u));
  }
  if (d->key != NULL) d->key(d->key_ctx, scancode, is_press);
}

/* Every key still down, released. */
static void release_keys(ikbd_demux_t *d) {
  for (uint8_t sc = 1; sc < 0x80u; sc++) {
    if (key_down(d, sc)) emit_key(d, sc, false);
  }
}

/* The guard needs the ticks to end: with no tick seen in this session (an
 * ST side that does not report its count), a loss only drops the open
 * packet. */
static void resync(ikbd_demux_t *d, ikbd_resync_cause_t cause) {
  d->need = 0;
  d->resyncs[cause]++;
  if (d->count_known && !d->guard) {
    d->guard = true;
    d->guards++;
  }
}

static void set_joy(ikbd_demux_t *d, unsigned port, uint8_t state) {
  d->joy_pressed[port] |= (uint8_t)(state & ~d->joy[port]);
  d->joy[port] = state;
}

static void packet_done(ikbd_demux_t *d) {
  uint8_t h = d->header;
  if (h >= 0xF8u && h <= 0xFBu) {
    d->mouse_packets++;
    if (d->guard) return;
    d->mouse_dx += (int8_t)d->payload[0];
    d->mouse_dy += (int8_t)d->payload[1];
    uint8_t buttons = (uint8_t)(h & (IKBD_MOUSE_LEFT | IKBD_MOUSE_RIGHT));
    d->mouse_pressed |= (uint8_t)(buttons & ~d->mouse_buttons);
    d->mouse_buttons = buttons;
    if (d->right_is_joy1_fire) {
      uint8_t fire = (buttons & IKBD_MOUSE_RIGHT) ? IKBD_JOY_FIRE : 0u;
      set_joy(d, 1, (uint8_t)((d->joy[1] & ~IKBD_JOY_FIRE) | fire));
    }
  } else if (h >= 0xFEu) {
    d->joy_events++;
    if (d->payload[0] & JOY_UNUSED_BITS) {
      resync(d, IKBD_RESYNC_JOYSTICK);
      return;
    }
    if (d->guard) return;
    unsigned port = h & 1u;
    uint8_t state = d->payload[0];
    /* With the mouse on, the IKBD reports stick 1's fire only as the right
     * button, never in the stick's events: keep what the button set. */
    if (port == 1u && d->right_is_joy1_fire) {
      state = (uint8_t)((state & ~IKBD_JOY_FIRE) | (d->joy[1] & IKBD_JOY_FIRE));
    }
    set_joy(d, port, state);
  } else if (h == 0xFDu) {
    d->joy_reports++;
    if ((d->payload[0] | d->payload[1]) & JOY_UNUSED_BITS) {
      resync(d, IKBD_RESYNC_JOYSTICK);
      return;
    }
    if (d->guard) return;
    /* With the mouse in port 0, stick 0's byte is the mouse's wires. */
    if (!d->right_is_joy1_fire) set_joy(d, 0, d->payload[0]);
    set_joy(d, 1, d->payload[1]);
  } else {
    d->other_packets++;
  }
}

static void feed(ikbd_demux_t *d, uint8_t b) {
  d->bytes++;
  if (d->need != 0u) {
    d->payload[d->got++] = b;
    if (--d->need == 0u) packet_done(d);
    return;
  }
  if (b >= 0xF6u) {
    d->header = b;
    d->need = k_payload_bytes[b - 0xF6u];
    d->got = 0;
    d->ticks = 0;
    /* Never asked for: most likely the stream is out of step. The packet
     * is still collected, so its bytes are not read as keys. */
    if (b == 0xF6u || b == 0xF7u || b == 0xFCu) {
      uint8_t need = d->need;
      resync(d, IKBD_RESYNC_UNEXPECTED);
      d->need = need;
    }
    return;
  }
  uint8_t scancode = (uint8_t)(b & 0x7Fu);
  if (scancode == 0u) return; /* not a key */
  if ((b == 0xF0u || b == 0xF1u) && !key_down(d, scancode)) {
    d->power_ups++;           /* the IKBD's power-up answer */
    release_keys(d);
    return;
  }
  if (b < 0x80u) {
    if (d->guard) {
      d->dropped_presses++;
      return;
    }
    emit_key(d, scancode, true);
  } else {
    emit_key(d, scancode, false); /* a release always passes */
  }
}

void ikbd_demux_init(ikbd_demux_t *d, ikbd_demux_key_fn key, void *ctx) {
  memset(d, 0, sizeof(*d));
  d->key = key;
  d->key_ctx = ctx;
}

void ikbd_demux_session(ikbd_demux_t *d) {
  d->need = 0;
  d->guard = false;
  d->count_known = false;
  d->right_is_joy1_fire = false;
  d->mouse_dx = 0;
  d->mouse_dy = 0;
  d->mouse_buttons = 0;
  d->mouse_pressed = 0;
  memset(d->joy, 0, sizeof(d->joy));
  memset(d->joy_pressed, 0, sizeof(d->joy_pressed));
}

void ikbd_demux_byte(ikbd_demux_t *d, uint8_t b) {
  d->count++;
  feed(d, b);
}

void ikbd_demux_injected(ikbd_demux_t *d, uint8_t b) { feed(d, b); }

void ikbd_demux_tick(ikbd_demux_t *d, uint8_t st_count) {
  bool quiet = d->count_known && st_count == d->st_count;
  if (d->count_known && st_count != d->count) {
    resync(d, IKBD_RESYNC_COUNT);
  }
  d->count = st_count;
  d->st_count = st_count;
  d->count_known = true;
  if (d->need != 0u && ++d->ticks >= 2u) {
    resync(d, IKBD_RESYNC_TIMEOUT);
  }
  /* The ST read no byte for a whole VBL: no packet can be open across it,
   * so the next byte starts something. */
  if (d->guard && quiet) {
    d->need = 0;
    d->guard = false;
  }
}

void ikbd_demux_loss(ikbd_demux_t *d, ikbd_resync_cause_t cause) {
  resync(d, cause);
}

void ikbd_demux_devices(ikbd_demux_t *d, bool mouse, bool joy0, bool joy1) {
  d->right_is_joy1_fire = mouse && joy1;
  if (!mouse) d->mouse_buttons = 0;
  if (!joy0) d->joy[0] = 0;
  if (!joy1) d->joy[1] = 0;
}

uint32_t ikbd_demux_resyncs(const ikbd_demux_t *d) {
  uint32_t n = 0;
  for (unsigned i = 0; i < IKBD_RESYNC_CAUSES; i++) n += d->resyncs[i];
  return n;
}
