/**
 * File: st_session.c
 * Description: The ST's hello at every boot; see st_session.h.
 */

#include "st_session.h"

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"

static uint8_t s_tos_hi;
static uint8_t s_tos_lo;
static uint8_t s_machine;
static uint16_t s_tos_version;
static bool s_boot_pending;
/* Hellos seen since the RP started; readable over SWD by its symbol. */
uint32_t stSessionHellos = 0;

void st_session_consume_rom3_sample(uint16_t sample) {
  uint8_t value = (uint8_t)(sample & 0xFFu);
  switch (sample & CART_ROM3_WINDOW_MASK) {
    case CART_ROM3_TOS_HI_WINDOW:
      s_tos_hi = value;
      break;
    case CART_ROM3_TOS_LO_WINDOW:
      s_tos_lo = value;
      break;
    case CART_ROM3_HELLO_WINDOW:
      /* The ST reads the sentinel only once its VBL loop runs, well after
       * this read (userfw first waits for the IKBD's reset answer). */
      *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                              CART_CMD_SENTINEL_OFFSET)) =
          cart_asM68kLong(CART_CMD_NOP);
      s_machine = value;
      s_tos_version = (uint16_t)((s_tos_hi << 8) | s_tos_lo);
      s_boot_pending = true;
      stSessionHellos++;
      DPRINTF("ST hello #%lu: machine 0x%02X, TOS %X.%02X\n",
              (unsigned long)stSessionHellos, (unsigned)s_machine,
              (unsigned)s_tos_hi, (unsigned)s_tos_lo);
      break;
    default:
      break;
  }
}

bool st_session_consume_boot(void) {
  bool booted = s_boot_pending;
  s_boot_pending = false;
  return booted;
}

uint8_t st_session_machine(void) { return s_machine; }

uint16_t st_session_tos_version(void) { return s_tos_version; }
