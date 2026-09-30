/**
 * File: st_session.c
 * Description: The ST's hello at every boot; see st_session.h.
 */

#include "st_session.h"

#include "cart_shared.h"
#include "constants.h"
#include "debug.h"
#include "gconfig.h"
#include "pico/stdlib.h"
#include "reset.h"

static uint8_t s_tos_hi;
static uint8_t s_tos_lo;
static uint8_t s_machine;
static uint16_t s_tos_version;
static bool s_boot_pending;
/* Hellos seen since the RP started, and the features the ST reported after
 * the last one; readable over SWD by their symbols. */
uint32_t stSessionHellos = 0;
uint32_t stSessionFeatures = 0;

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
      stSessionFeatures = 0;
      s_boot_pending = true;
      stSessionHellos++;
      DPRINTF("ST hello #%lu: machine 0x%02X, TOS %X.%02X\n",
              (unsigned long)stSessionHellos, (unsigned)s_machine,
              (unsigned)s_tos_hi, (unsigned)s_tos_lo);
      break;
    case CART_ROM3_ST_FEATURES_WINDOW:
      stSessionFeatures = value;
      DPRINTF("ST features 0x%02X\n", (unsigned)value);
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

uint32_t st_session_hellos(void) { return stSessionHellos; }

uint16_t st_session_tos_version(void) { return s_tos_version; }

uint8_t st_session_features(void) { return (uint8_t)stSessionFeatures; }

void st_session_veto_boot(const char *reason) {
  volatile uint8_t *message =
      (volatile uint8_t *)((uintptr_t)&__rom_in_ram_start__ +
                           CART_BOOT_MESSAGE_OFFSET);
  /* The cart bus swaps the two bytes of each 16-bit word: the ST's byte at
   * an even address is the RP's byte at the odd one, and back. */
  size_t i = 0;
  for (; reason[i] != '\0' && i < CART_BOOT_MESSAGE_SIZE - 1; i++) {
    message[i ^ 1u] = (uint8_t)reason[i];
  }
  message[i ^ 1u] = 0;
  /* The status last, so the ST never prints a half-written message. */
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_BOOT_STATUS_OFFSET)) = CART_BOOT_VETOED;
  DPRINTF("ST boot vetoed: %s\n", reason);
}

void st_session_allow_boot(void) {
  *((volatile uint16_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_BOOT_STATUS_OFFSET)) = CART_BOOT_OK;
}

void st_session_return_to_booster(void) {
  /* Not reset_jump_to_booster(): that belongs at the top of main(), before
   * anything runs. Here the bus emulation, its DMA and core 1 are live, and
   * Booster would inherit them and hang. After a restart, main()'s
   * gconfig_init() finds another app in BOOT_FEATURE and takes the jump
   * there. */
  DPRINTF("Returning to Booster\n");
  settings_put_string(gconfig_getContext(), PARAM_BOOT_FEATURE, "BOOSTER");
  settings_save(gconfig_getContext(), true);
  /* The ST polls the sentinel once per VBL, then waits a couple of seconds
   * in its own RAM and cold-resets, by when Booster serves the cartridge. */
  *((volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                          CART_CMD_SENTINEL_OFFSET)) =
      cart_asM68kLong(CART_CMD_RESET);
  sleep_ms(100); /* several VBLs: the ST must see it before the bus goes */
  reset_device();
}
