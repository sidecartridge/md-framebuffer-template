/**
 * File: ikbd.h
 * Description: The ST's keyboard, mouse and joysticks, decoded on the RP.
 *
 * The m68k keyboard ACIA interrupt (userfw_acia_irq in
 * target/atarist/src/userfw.s) forwards every byte the IKBD sends with a
 * ROM3 read at $FB8200 + byte, and every VBL reports how many it has read
 * ($FB8300 + count). `ikbd_consume_rom3_sample()`, commemul_poll's callback
 * (through fb.c), keeps those samples in order; `ikbd_pump()`, once per
 * main-loop iteration, decodes them (ikbd_demux.c) into key events, mouse
 * movement and buttons, and joystick states.
 *
 * Input modes. Port 0 is the mouse or joystick 0, never both, so the app
 * picks a mode: keyboard only (the default), mouse, mouse + joystick 1, or
 * two joysticks. The keyboard works in every mode. ikbd_set_input_mode()
 * puts that mode's IKBD commands in the cartridge window; the ST sends them
 * one per VBL and reports when it has, a few VBLs later
 * (ikbd_get_live_input_mode()). The mode the app asked for survives an ST
 * reset: it is sent again after every boot. ikbd_send_commands() sends
 * other IKBD commands the same way.
 *
 * Hardware facts an app has to live with:
 *   - Stick 0's fire is the left mouse button and stick 1's fire the right
 *     one: the same wires. In mouse + joystick 1 mode the right button is
 *     also stick 1's fire (ikbd_read_joystick(1) reports it).
 *   - A mouse left in port 0 in two-joystick mode moves stick 0: its wires
 *     are the stick's switches. That is noise from the mouse, not a bug.
 *   - The IKBD has no auto-repeat: one press and one release per key.
 *
 * Keys: ikbd_pop_key(). ESC press+release within 200 ms exits to GEM
 * (CMD_BOOT_GEM) unless the app turns that off (ikbd_set_esc_auto_exit()).
 * Mouse: ikbd_read_mouse(), movement since the last read and the buttons.
 * Joysticks: ikbd_read_joystick(). Both keep "pressed since the last read"
 * latches, so a click or a tap shorter than a frame is never missed.
 *
 * When bytes are lost (never seen so far: the ACIA reports it, the counts
 * catch the rest), the demux drops what it cannot trust until the stream
 * is back in step: see ikbd_demux.h. When the IKBD restarts (a keyboard
 * plugged back in), the keys still down are released and the input mode is
 * sent again.
 */

#ifndef IKBD_H_INCLUDED
#define IKBD_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cart_shared.h"
#include "ikbd_demux.h" /* IKBD_MOUSE_* and IKBD_JOY_* */

#ifdef __cplusplus
extern "C" {
#endif

/* Ring + filter constants exposed for tests / debug code. */
#define IKBD_RING_CAPACITY 256u
#define IKBD_WINDOW_LO16   0x8200u  /* low 16 bits of $FB8200 */
#define IKBD_WINDOW_MASK   0xFF00u  /* discriminator: high byte == 0x82 */

/* Reset the ring, the decoder and the mode (keyboard). Call once at boot,
 * after the cartridge window has been set up. */
void ikbd_init(void);

/* commemul ring consumer. Passed as the callback to commemul_poll
 * for every ROM3 sample. Keeps the IKBD bytes, the ST's byte counts, its
 * overruns and input mode reports and its hello, in order. Runs in
 * main-loop context. */
void ikbd_consume_rom3_sample(uint16_t addr_lsb);

/* An IKBD byte from a host tool (debug builds, devhooks.h): decoded like
 * the ST's, but the ST never counted it. */
void ikbd_inject_byte(uint8_t b);

/* Decode what arrived: key events onto the key ring, mouse and joystick
 * state for the readers below. Also signals CMD_BOOT_GEM via the cart
 * command sentinel on ESC press+release. Call once per main-loop
 * iteration. */
void ikbd_pump(void);

/* Approximate number of raw samples currently in the ring. */
size_t ikbd_ring_count(void);

/* Cumulative count of raw samples the producer couldn't push because
 * the ring was full. Should stay 0 in steady state. */
uint32_t ikbd_ring_dropped(void);

/* Times the ST's keyboard ACIA reported an overrun (IKBD bytes lost
 * before they could be read), signalled at CART_ROM3_IKBD_OVERRUN_WINDOW.
 * Should stay 0; debug builds print each one. */
uint32_t ikbd_overruns(void);

/* The decoder, for its counters (bytes, packets, resyncs by cause). */
const ikbd_demux_t *ikbd_demux(void);

/* Key press / release event. `scancode` is the IKBD scancode with
 * bit 7 stripped (1..127). `is_press` is true for make, false for
 * break. */
typedef struct {
  uint8_t scancode;
  bool is_press;
} ikbd_key_event_t;

/* Pop the next decoded key event. Returns false if the ring is
 * empty. */
bool ikbd_pop_key(ikbd_key_event_t *out);

/* Enable / disable the built-in ESC press+release -> CMD_BOOT_GEM
 * sentinel write. Default = true (backward-compatible with the
 * ergonomic of "press ESC to exit"). Apps that want to own
 * the ESC key (e.g. the menu+demo dispatcher that uses ESC for
 * "back to menu") call ikbd_set_esc_auto_exit(false) once at boot.
 * ESC events are still delivered via ikbd_pop_key() regardless of
 * the setting. */
void ikbd_set_esc_auto_exit(bool enabled);

/* Input modes (see above). */
typedef enum {
  IKBD_INPUT_KEYBOARD = 0,   /* keys only */
  IKBD_INPUT_MOUSE = 1,      /* + mouse in port 0 */
  IKBD_INPUT_MOUSE_JOY1 = 2, /* + mouse in port 0, stick in port 1 */
  IKBD_INPUT_JOYSTICKS = 3,  /* + sticks in ports 0 and 1 */
} ikbd_input_mode_t;
#define IKBD_INPUT_MODES 4u

/* Ask the ST for this input mode; anything else is ignored. */
void ikbd_set_input_mode(ikbd_input_mode_t mode);

/* Send the IKBD these command bytes (at most CART_IKBD_OUT_MAX; a 0 is a
 * VBL with nothing sent), one per VBL. Replaces commands the ST has not
 * finished sending, a mode switch's included, and is not sent again after
 * an ST reset: for settings the modes do not cover, once the mode is live.
 * False when too long. */
bool ikbd_send_commands(const uint8_t *cmd, size_t n);

/* The mode the app asked for last. */
ikbd_input_mode_t ikbd_get_input_mode(void);

/* The mode the ST last reported the IKBD in, or -1 while it has not
 * reported one since it booted. It equals ikbd_get_input_mode() once the
 * switch is done. */
int ikbd_get_live_input_mode(void);

/* The mouse: movement since the last read (dx right, dy down), the buttons
 * held now and the buttons pressed since the last read (IKBD_MOUSE_LEFT,
 * IKBD_MOUSE_RIGHT). Reading clears the movement and `pressed`. */
typedef struct {
  int16_t dx;
  int16_t dy;
  uint8_t buttons;
  uint8_t pressed;
} ikbd_mouse_t;

void ikbd_read_mouse(ikbd_mouse_t *out);

/* A joystick (port 0 or 1): the switches closed now and those that closed
 * since the last read (IKBD_JOY_UP, _DOWN, _LEFT, _RIGHT, _FIRE). Reading
 * clears `pressed`. All zero in a mode without that stick. */
typedef struct {
  uint8_t state;
  uint8_t pressed;
} ikbd_joystick_t;

void ikbd_read_joystick(unsigned port, ikbd_joystick_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IKBD_H_INCLUDED */
