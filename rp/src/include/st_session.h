/**
 * File: st_session.h
 * Description: The ST's sessions, as the RP sees them.
 *
 * The ST and the RP reboot independently, and the RP keeps its state across
 * an ST reset: the command sentinel, the app's own state. Without a signal
 * from the ST, an exit to GEM leaves CMD_BOOT_GEM in the sentinel, and the
 * next ST boot runs userfw, which reads it and leaves at once.
 *
 * So userfw says hello at every boot, with three ROM3 reads (the windows are
 * listed in cart_shared.h): the TOS version's two bytes, then the hello
 * itself, whose low byte is the machine. On the hello this module writes
 * CMD_NOP to the sentinel, before the ST's VBL loop first reads it, and
 * flags a new session for the app, which starts its own state over.
 *
 * Machine byte, from the _MCH cookie ($0000 on a TOS without a cookie jar):
 * family in bits 7..4, sub-model in bits 3..0.
 *   0x00 ST / Mega ST, 0x10 STE, 0x11 Mega STE, 0x20 TT, 0x30 Falcon
 */

#ifndef ST_SESSION_H
#define ST_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#define ST_MACHINE_ST 0x00u
#define ST_MACHINE_STE 0x10u
#define ST_MACHINE_MEGASTE 0x11u
#define ST_MACHINE_TT 0x20u
#define ST_MACHINE_FALCON 0x30u

/* commemul ring consumer: handles the hello windows, ignores the rest. Runs
 * in main-loop context (fb_pump_rom3). */
void st_session_consume_rom3_sample(uint16_t sample);

/* True once after each ST boot, then false until the next one. */
bool st_session_consume_boot(void);

/* The machine byte and the TOS version (e.g. 0x0206) of the last hello;
 * 0 before the first one. */
uint8_t st_session_machine(void);
uint16_t st_session_tos_version(void);

/* What the ST reported having after the last hello (CART_ST_FEATURE_*:
 * a blitter); 0 until it has. */
uint8_t st_session_features(void);

/* The hellos seen since the RP started: a module that must start over with
 * each ST boot compares it with the count it last saw (audio.c). */
uint32_t st_session_hellos(void);

/* Refuses to start the app on the ST: pre_auto prints `reason` and returns to
 * GEM, as it does in high resolution. At most CART_BOOT_MESSAGE_SIZE - 1
 * characters; "\r\n" breaks a line, and the ST adds one at the end. The ST
 * reads it once per boot, before userfw starts (on a power-on about 0.3 s
 * after the RP starts): set it later and it applies from the next ST reset.
 * It holds until st_session_allow_boot(), so the ST can be reset again once
 * the cause is gone (a card inserted, say). */
void st_session_veto_boot(const char *reason);
void st_session_allow_boot(void);

/* Leaves the app for Booster, without a power cycle: saves BOOT_FEATURE =
 * BOOSTER in the global settings, asks the ST for a cold reset and restarts
 * the RP, whose main() then jumps to Booster. Never returns. The ST acts on
 * it only while userfw runs; from GEM, reset the ST by hand. It writes flash:
 * call it from the main loop, with no job running on core 1. */
void st_session_return_to_booster(void);

#endif /* ST_SESSION_H */
