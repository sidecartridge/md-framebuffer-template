/**
 * File: ikbd.h
 * Description: IKBD keyboard ingest + demux (keyboard-only).
 *
 * The m68k Timer-B IKBD handler (target/atarist/src/userfw.s) reads
 * the keyboard ACIA at $FFFFFC00/02 and forwards every received byte
 * to the RP via a single cart-bus read at IKBD_WINDOW_BASE + byte in
 * the ROM3 region ($FB8200..$FB82FF, md-devops single-byte ABI).
 *
 * `ikbd_consume_rom3_sample(addr_lsb)` is passed directly as the
 * callback to `commemul_poll` in the emul.c main loop, filters for
 * the 256 B IKBD window, and pushes the low byte into a 64-entry
 * raw-byte ring.
 *
 * `ikbd_pump()` drains the raw-byte ring on the main loop and
 * classifies each byte statelessly:
 *   $00..$7F → key press scancode (scancode 0 suppressed)
 *   $80..$F1 → key release scancode (byte & $7F)
 *   $F2..$FF → packet headers for mouse / joystick / status / TOD.
 *              Mouse and joystick are disabled at IKBD boot ($12
 *              and $1A), so these shouldn't arrive in steady state.
 *              Discarded as single bytes; any leaked follow bytes
 *              may emit one-shot spurious key events.
 *
 * Apps drain decoded key events via `ikbd_pop_key`. ESC (scancode
 * $01) press+release within 200 ms triggers CMD_BOOT_GEM via the
 * cart command sentinel to exit userfw cleanly.
 *
 * Mouse and joystick decoding were both attempted but
 * proved unreliable (same byte-loss / demux-desync class of bug)
 * and are deferred.
 * Apps that want them can drop the $12 / $1A IKBD commands in
 * userfw.s and add their own packet-emit branches in this demux.
 */

#ifndef IKBD_H_INCLUDED
#define IKBD_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring + filter constants exposed for tests / debug code. */
#define IKBD_RING_CAPACITY 64u
#define IKBD_WINDOW_LO16   0x8200u  /* low 16 bits of $FB8200 */
#define IKBD_WINDOW_MASK   0xFF00u  /* discriminator: high byte == 0x82 */

/* Reset the ring buffer + cached state. Safe to call multiple times. */
void ikbd_init(void);

/* commemul ring consumer. Passed as the callback to commemul_poll
 * for every ROM3 sample. Filters for the IKBD window, extracts the
 * low byte, and pushes it onto the raw-byte ring. Runs in main-loop
 * context. */
void ikbd_consume_rom3_sample(uint16_t addr_lsb);

/* Drain the raw-byte ring, feeding bytes through the keyboard demux
 * and pushing decoded events onto the key event ring. Also signals
 * CMD_BOOT_GEM via the cart command sentinel on ESC press+release.
 * Call once per main-loop iteration. */
void ikbd_pump(void);

/* Approximate number of raw bytes currently in the ring. */
size_t ikbd_ring_count(void);

/* Cumulative count of raw bytes the producer couldn't push because
 * the ring was full. Should stay 0 in steady state. */
uint32_t ikbd_ring_dropped(void);

/* Key press / release event. `scancode` is the IKBD scancode with
 * bit 7 stripped (0..127). `is_press` is true for make, false for
 * break. Scancode $00 is suppressed (not a valid IKBD key). */
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

#ifdef __cplusplus
}
#endif

#endif /* IKBD_H_INCLUDED */
