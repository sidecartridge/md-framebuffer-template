/**
 * File: display.c
 * Author: Diego Parrilla Santamaría
 * Date: December 2024, February 2026
 * Copyright: 2024-2026 - GOODDATA LABS SL
 * Description: Common functions for the display
 */

#include "display.h"

static uint32_t displayAddress = 0;
static uint32_t displayCommandAddress = 0;
static uint32_t displaysHighresTranstableAddress = 0;

// Static assert to ensure buffer size fits within uint32_t
_Static_assert(DISPLAY_BUFFER_SIZE <= UINT32_MAX,
               "Buffer size exceeds allowed limits");

// Allocate the framebuffer
#if DISPLAY_BYPASS_FRAMEBUFFER == 0
static unsigned char u8g2Buffer[DISPLAY_BUFFER_SIZE] = {0};
#else
static unsigned char *u8g2Buffer = NULL;
#endif
// Global u8g2 structure
static u8g2_t u8g2 = {0};

// Dummy byte communication function
static unsigned char u8x8DummyByte(void *u8x8, unsigned char msg,
                                   unsigned char argInt, void *argPtr) {
  return 1;  // Always return success
}

// Dummy GPIO function
static unsigned char u8x8DummyGpio(void *u8x8, unsigned char msg,
                                   unsigned char argInt, void *argPtr) {
  return 1;  // Always return success
}

// Dummy Command/Data function
static unsigned char u8x8CadDummy(void *u8x8, unsigned char msg,
                                  unsigned char argInt, void *argPtr) {
  return 1;  // Always return success
}

#ifdef DISPLAY_ATARIST
static const u8x8_display_info_t u8x8Ataristlow320x200DisplayInfo = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,

    /* post_chip_enable_wait_ns = */ 30, /* G242CX Datasheet p5 */
    /* pre_chip_disable_wait_ns = */ 10, /* G242CX Datasheet p5 */
    /* reset_pulse_width_ms = */ 1,
    /* post_reset_wait_ms = */ 6,
    /* sda_setup_time_ns = */ 20,
    /* sck_pulse_width_ns = */ 140,
    /* sck_clock_hz = */ 1000000UL, /* since Arduino 1.6.0, the SPI bus speed in
                                       Hz. Should be
                                       1000000000/sck_pulse_width_ns */
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 4,
    /* data_setup_time_ns = */ 120,   /* G242CX Datasheet p5 */
    /* write_pulse_width_ns = */ 220, /* G242CX Datasheet p5 */
    /* tile_width = */ 40,
    /* tile_height = */ 25,
    /* default_x_offset = */ 0,
    /* flipmode_x_offset = */ 0,
    /* pixel_width = */ 320,
    /* pixel_height = */ 200};

// Getter function for u8g2 structure
u8g2_t *display_getU8g2Ref() { return &u8g2; }

// Getter function for display address
uint32_t display_getAddress() { return displayAddress; }

// Setter function for display address
static void setDisplayAddress(uint32_t address) { displayAddress = address; }

// Getter function for display command address
uint32_t display_getCommandAddress() { return displayCommandAddress; }

// Setter function for display command address
static void setDisplayCommandAddress(uint32_t address) {
  displayCommandAddress = address;
}

// Getter function for highres translation table address
uint32_t display_getHighresTranstableAddress() {
  return displaysHighresTranstableAddress;
}

// Setter function for highres translation table address
static void setDisplaysHighresTranstableAddress(uint32_t address) {
  displaysHighresTranstableAddress = address;
}

unsigned char u8x8DCustom(u8x8_t *u8x8, unsigned char msg, unsigned char argInt,
                          void *argPtr) {
  if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
    u8x8_d_helper_display_setup_memory(u8x8, &u8x8Ataristlow320x200DisplayInfo);
  }
  return 1;
}
#endif

// Initialize u8g2 with the custom buffer
void display_setupU8g2() {
  DPRINTF("Display Bypass framebuffer? %s\n",
          DISPLAY_BYPASS_FRAMEBUFFER ? "YES" : "NO");

#if DISPLAY_BYPASS_FRAMEBUFFER == 1
  u8g2Buffer =
      (void *)((unsigned int)&__rom_in_ram_start__ + DISPLAY_BUFFER_OFFSET);
#else
  DPRINTF("Initializing u8g2 with a buffer size of %d bytes\n",
          DISPLAY_BUFFER_SIZE);
#endif

  setDisplayAddress((unsigned int)&__rom_in_ram_start__ +
                    DISPLAY_BUFFER_OFFSET);
  setDisplayCommandAddress((unsigned int)&__rom_in_ram_start__ +
                           DISPLAY_COMMAND_ADDRESS);
  setDisplaysHighresTranstableAddress((unsigned int)&__rom_in_ram_start__ +
                                      DISPLAY_HIGHRES_TRANSTABLE_ADDR);
  DPRINTF("Display buffer address: 0x%08x\n", (unsigned int)u8g2Buffer);
  DPRINTF("Display command address: 0x%08x\n", display_getCommandAddress());
  DPRINTF("Highres translation table address: 0x%08x\n",
          display_getHighresTranstableAddress());

#ifdef DISPLAY_ATARIST
  // We need to generate the mask table for the Atari ST display (faster highres
  // mode)
  display_generateMaskTable(display_getHighresTranstableAddress());

  // We clear the command address just in case
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_NOP);
#endif

  u8g2_SetupDisplay(&u8g2, u8x8DCustom, (u8x8_msg_cb)u8x8CadDummy,
                    (u8x8_msg_cb)u8x8DummyByte, (u8x8_msg_cb)u8x8DummyGpio);

  // Calculate tile buffer height
  uint8_t tileBufHeight = DISPLAY_HEIGHT / DISPLAY_TILE_HEIGHT;

  u8g2_SetupBuffer(&u8g2, u8g2Buffer, tileBufHeight,
                   u8g2_ll_hvline_horizontal_right_lsb, U8G2_R0);

  // Fake initialization sequence
  u8g2_InitDisplay(&u8g2);  // Initialize display (will use dummy callbacks)
}

void display_refresh() {
#if DISPLAY_BYPASS_FRAMEBUFFER == 0
  uint32_t *displayBuffer = (void *)display_getAddress();
  COPY_AND_SWAP_16BIT_DMA(displayBuffer, (uint16_t *)u8g2Buffer,
                          DISPLAY_BUFFER_SIZE);
#endif
}

void display_drawProductInfo() {
  // Product info
  char productStr[DISPLAY_MAX_CHARACTERS] = {0};
  u8g2_SetFont(&u8g2, u8g2_font_squeezed_b7_tr);
  snprintf(productStr, sizeof(productStr), "%s %s - %s", DISPLAY_PRODUCT_MSG,
           RELEASE_VERSION, DISPLAY_COPYRIGHT_MESSAGE);
  u8g2_DrawStr(
      &u8g2,
      LEFT_PADDING_FOR_CENTER(productStr, 68) * DISPLAY_NARROW_CHAR_WIDTH,
      DISPLAY_HEIGHT, productStr);
}

void display_generateMaskTable(uint32_t memoryAddress) {
  for (int i = 0; i < DISPLAY_MASK_TABLE_SIZE; i++) {
    unsigned int mask = 0;

    // Duplicate each bit of the 8-bit number into two bits
    for (int bit = 0; bit < DISPLAY_MASK_TABLE_CHAR; bit++) {
      if (i & (1 << bit)) {
        mask |= (3 << (2 * bit));  // Duplicate the bit at position b
      }
    }

    // Store the 16 bit result mask in the memory address
#if DISPLAY_HIGHRES_INVERT == 1
    WRITE_WORD(memoryAddress, i * 2, ~mask);
#else
    WRITE_WORD(memoryAddress, i * 2, mask);
#endif
  }
}

// Scroll up the display buffer by blanking out the bottom part
// blank_bytes is the number of bytes to blank out at the bottom of the screen
// They should be the same as the number of bytes in a row of chars
void display_scrollup(uint16_t blankBytes) {
  // blank bytes is the number of bytes to blank out at the bottom of the screen
  if (blankBytes == 0) {
    return;
  }

  if (blankBytes >= DISPLAY_BUFFER_SIZE) {
    memset(u8g2Buffer, 0, DISPLAY_BUFFER_SIZE);
    return;
  }

  memmove(u8g2Buffer, u8g2Buffer + blankBytes,
          DISPLAY_BUFFER_SIZE - blankBytes);
  memset(u8g2Buffer + DISPLAY_BUFFER_SIZE - blankBytes, 0, blankBytes);
}
