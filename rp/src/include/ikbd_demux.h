/**
 * File: ikbd_demux.h
 * Description: The IKBD byte stream -> keys, mouse and joysticks.
 *
 * Plain C, no SDK: tests/host/test_ikbd_demux.c builds it as is. ikbd.c
 * feeds it what the ST reports, in the order the ST reported it: every IKBD
 * byte, the byte count the ST reports every VBL (the "tick"), the ACIA's
 * overruns, the devices the IKBD reports, a new ST session.
 *
 * The IKBD sends keys and packets on one serial line, one byte every
 * 1.28 ms. A byte $00-$7F is a key press, $80-$F5 a key release, $F6-$FF a
 * packet header followed by a fixed number of bytes:
 *
 *   $F6 status 7   $F7 absolute mouse 5   $F8-$FB relative mouse 2 (dx, dy)
 *   $FC clock 6    $FD both joysticks 2   $FE / $FF joystick 0 / 1 event 1
 *
 * Nothing in the stream marks where a packet ends, so a lost byte shifts
 * everything after it: the tail of a mouse packet decodes as keys (dx = +1
 * is ESC). What the demux does about it:
 *
 *   - It knows when bytes were lost: the ACIA reported an overrun, the RP's
 *     ring was full, the count at a tick is not the count of bytes that
 *     arrived, a packet is still open at its second tick (the longest takes
 *     10 ms, ticks come every 20), a joystick byte has bits 4-6 set, or a
 *     packet this template never asks for arrives ($F6, $F7, $FC).
 *   - Then it drops the open packet and enters the guard: key presses,
 *     mouse and joystick data are dropped, key releases still pass (a key
 *     never stays down). The stream is certainly back in step at a tick
 *     where the ST's count says it read no byte since the tick before (the
 *     IKBD sends a packet's bytes back to back), and the guard ends there.
 *     A mouse moved without a pause keeps it on; a pause of one frame is
 *     enough. The RP's own view of the frame is not: bytes its ring
 *     dropped would make a busy frame look quiet.
 *
 * Each ACIA overrun is reported just before the byte read with it, so the
 * guard is on before the damaged bytes arrive. The count check comes at the
 * next tick: a loss only it sees (none is known) could show as one wrong
 * key before it.
 *
 * The IKBD powering up again (a keyboard unplugged and plugged back) says
 * $F0 or $F1, the release codes of keypad 0 and keypad '.': the demux keeps
 * which keys are down and takes either byte for the power-up when that key
 * is not. Then every key still down is released (its release was lost with
 * the keyboard) and power_ups counts it: the IKBD is back in its own mode,
 * so the input mode must be sent again (its $16 reports the sticks; the
 * mouse's next packet, its buttons). A keypad 0 / '.' press lost to damage
 * makes its release look like a power-up: the keys down are released.
 */

#ifndef IKBD_DEMUX_H_INCLUDED
#define IKBD_DEMUX_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse buttons, as the header of a mouse packet carries them. */
#define IKBD_MOUSE_RIGHT 0x01u
#define IKBD_MOUSE_LEFT  0x02u

/* Joystick state bits, as the IKBD sends them. */
#define IKBD_JOY_UP    0x01u
#define IKBD_JOY_DOWN  0x02u
#define IKBD_JOY_LEFT  0x04u
#define IKBD_JOY_RIGHT 0x08u
#define IKBD_JOY_FIRE  0x80u

/* Why the demux dropped what it had (ikbd_demux_t.resyncs[]). */
typedef enum {
  IKBD_RESYNC_OVERRUN = 0,  /* the ST's ACIA lost bytes */
  IKBD_RESYNC_DROPPED,      /* the RP's ring was full */
  IKBD_RESYNC_COUNT,        /* the ST read more or fewer bytes than arrived */
  IKBD_RESYNC_TIMEOUT,      /* a packet still open at its second tick */
  IKBD_RESYNC_JOYSTICK,     /* a joystick byte with bits 4-6 set */
  IKBD_RESYNC_UNEXPECTED,   /* a status, absolute mouse or clock packet */
  IKBD_RESYNC_CAUSES
} ikbd_resync_cause_t;

/* A key press or release, in stream order. Never scancode 0. */
typedef void (*ikbd_demux_key_fn)(void *ctx, uint8_t scancode, bool is_press);

typedef struct {
  /* The packet being collected. need = 0: between packets. */
  uint8_t header;
  uint8_t need;
  uint8_t got;
  uint8_t payload[7];
  uint8_t ticks;          /* ticks since the header */

  /* Loss handling (see above). */
  bool guard;
  bool count_known;       /* false until the first tick of a session */
  uint8_t count;          /* bytes seen, mod 256, against the ST's count */
  uint8_t st_count;       /* the ST's count at the last tick */

  /* The mouse + joystick 1 mode: the right button is stick 1's fire. */
  bool right_is_joy1_fire;

  /* The keys down now, one bit per scancode. */
  uint8_t keys_down[16];

  /* What the app reads (ikbd.c). Deltas and the "pressed" latches build up
   * until the app reads and clears them. */
  int32_t mouse_dx;
  int32_t mouse_dy;
  uint8_t mouse_buttons;
  uint8_t mouse_pressed;
  uint8_t joy[2];
  uint8_t joy_pressed[2];

  /* Counters. */
  uint32_t bytes;
  uint32_t keys;
  uint32_t mouse_packets;
  uint32_t joy_events;    /* $FE / $FF */
  uint32_t joy_reports;   /* $FD */
  uint32_t other_packets; /* $F6 / $F7 / $FC */
  uint32_t resyncs[IKBD_RESYNC_CAUSES];
  uint32_t guards;        /* times the guard began */
  uint32_t dropped_presses;
  uint32_t power_ups;     /* the IKBD said $F0 / $F1: it restarted */

  ikbd_demux_key_fn key;
  void *key_ctx;
} ikbd_demux_t;

/* Empty state, counters at zero; `key` gets every key event. */
void ikbd_demux_init(ikbd_demux_t *d, ikbd_demux_key_fn key, void *ctx);

/* A new ST session: the IKBD was reset. Drops the open packet and the
 * device state, keeps the counters; the next tick sets the count. */
void ikbd_demux_session(ikbd_demux_t *d);

/* An IKBD byte the ST read and counted. */
void ikbd_demux_byte(ikbd_demux_t *d, uint8_t b);

/* An IKBD byte the ST never read (a host tool typing): decoded like the
 * others, left out of the count. */
void ikbd_demux_injected(ikbd_demux_t *d, uint8_t b);

/* The ST's tick: its count of IKBD bytes read so far, mod 256. */
void ikbd_demux_tick(ikbd_demux_t *d, uint8_t st_count);

/* Bytes were lost at this point of the stream. */
void ikbd_demux_loss(ikbd_demux_t *d, ikbd_resync_cause_t cause);

/* The IKBD reports these devices now: the others are released. With the
 * mouse and stick 1 both, the right button is stick 1's fire (one wire). */
void ikbd_demux_devices(ikbd_demux_t *d, bool mouse, bool joy0, bool joy1);

/* Every resync, all causes. */
uint32_t ikbd_demux_resyncs(const ikbd_demux_t *d);

#ifdef __cplusplus
}
#endif

#endif /* IKBD_DEMUX_H_INCLUDED */
