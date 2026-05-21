/**
 * File: display.h
 * Author: Diego Parrilla Santamaría
 * Date: December 2024, February 2026
 * Copyright: 2024-2026 - GOODDATA LABS SL
 * Description: Header file for the shared displat functions
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "debug.h"
#include "hardware/dma.h"
#include "memfunc.h"
#include "u8g2.h"

// Define custom display dimensions

// For Atari ST display
#ifdef DISPLAY_ATARIST
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 200
#define DISPLAY_TILES_WIDTH 40
#define DISPLAY_TILES_HEIGHT 25
#define DISPLAY_TILE_HEIGHT 8
#define DISPLAY_TILE_WIDHT 8
#define DISPLAY_MAX_CHARACTERS 80
#define DISPLAY_NARROW_CHAR_WIDTH 6

#define DISPLAY_MASK_TABLE_SIZE 256
#define DISPLAY_MASK_TABLE_CHAR 8

// If 1, the display will not use the framebuffer and will write directly to the
// display memory. This is useful to reduce the memory usage.
// When not using the framebuffer, the endianess swap must be done in the remote
// computer.
#define DISPLAY_BYPASS_FRAMEBUFFER 1

// #define DISPLAY_COMMAND_ADDRESS (ROM_IN_RAM_ADDRESS + 0x10000 + 8000) //
// increment 64K bytes to get the second 64K block + 8000 bytes to get the 8K
// block #define DISPLAY_HIGHRES_TRANSTABLE_ADDRESS (ROM_IN_RAM_ADDRESS +
// 0x1000) // increment 4K bytes to create the translation table
#define DISPLAY_HIGHRES_INVERT \
  0  // If 1, the highres display will be inverted, otherwise it will be normal
#define DISPLAY_BYPASS_MESSAGE "Press any SHIFT key to boot from GEMDOS."
#define DISPLAY_TARGET_COMPUTER_NAME "Atari ST"
#endif

// Buffer size calculation: width * (height / 8)
#define DISPLAY_BUFFER_SIZE \
  (uint32_t)((DISPLAY_WIDTH / DISPLAY_TILE_HEIGHT) * DISPLAY_HEIGHT)
#define DISPLAY_COPYRIGHT_MESSAGE "(C)GOODDATA LABS SL 2023-26"
#define DISPLAY_PRODUCT_MSG "SidecarTridge Multi-Device"
#define DISPLAY_RESET_WAIT_MESSAGE "Resetting the computer"
#define DISPLAY_RESET_FORCE_MESSAGE "Reset manually if it doesn't boot."

// Display offsets are derived from the shared-region layout owned by
// chandler.h. The framebuffer sits at the top of the 64 KB region; the
// command sentinel and the high-res translation table both live in the
// shared block at the bottom of the region.
#include "chandler.h"

#define DISPLAY_BUFFER_OFFSET           CHANDLER_FRAMEBUFFER_OFFSET
#define DISPLAY_COMMAND_ADDRESS         CHANDLER_CMD_SENTINEL_OFFSET
#define DISPLAY_HIGHRES_TRANSTABLE_ADDR CHANDLER_APP_BUFFERS_OFFSET

// Commands sent to the active loop in the display terminal application
#define DISPLAY_COMMAND_NOP 0x0       // Do nothing, clean the command buffer
#define DISPLAY_COMMAND_RESET 0x1     // Reset the computer
#define DISPLAY_COMMAND_CONTINUE 0x2  // Continue the boot process
#define DISPLAY_COMMAND_TERMINAL \
  0x3                              //  Terminal. Not used from RP to Computer.
#define DISPLAY_COMMAND_START 0x4  // Continue boot process and emulation

/**
 * @brief Sends a command to the display.
 *
 * @param command The command to be sent to the display.
 */
#define SEND_COMMAND_TO_DISPLAY(command)                              \
  do {                                                                \
    DPRINTF("Sending command: %08x\n", command);                      \
    WRITE_AND_SWAP_LONGWORD(display_getCommandAddress(), 0, command); \
  } while (0)

/**
 * @brief Computes the left padding needed to center a string in a line.
 *
 * @param STR  The string to center.
 * @param WIDTH The total number of characters in the line.
 *
 * @return The number of spaces (left padding) for centering the string.
 */
#define LEFT_PADDING_FOR_CENTER(STR, WIDTH) \
  (((WIDTH) > strlen(STR)) ? (((WIDTH) - (size_t)strlen(STR)) / 2) : 0)

/**
 * @brief Initializes the u8g2 display with a custom buffer.
 *
 * Sets up the display by:
 * - Initializing the display, command, and high-resolution translation table
 * addresses using fixed offsets.
 * - Configuring u8g2 with custom callbacks for dummy byte, GPIO, and
 * command/data routines.
 * - Establishing buffer parameters and running a fake initialization sequence.
 */
void display_setupU8g2();

/**
 * @brief Refreshes the display.
 *
 * Copies the contents of the u8g2 buffer into the display's memory-mapped
 * buffer using a DMA transfer with 16-bit swapping, ensuring the on-screen
 * content is updated.
 */
void display_refresh();

/**
 * @brief Draws product information on the display.
 *
 * Sets the appropriate font, composes a string with the product message,
 * release version, and copyright information, centers it using calculated left
 * padding, and renders the text.
 */
void display_drawProductInfo();

/**
 * @brief Generates a high-resolution mask table. Used to speed up high-res
 * upscaled display.
 *
 * Creates a mask table by duplicating each bit of an 8-bit number into two
 * bits, producing a 16-bit value. Depending on the inversion setting, the
 * generated mask may be bitwise inverted before storage with WRITE_WORD.
 *
 * @param memoryAddress The memory address where the mask table will be stored.
 */
void display_generateMaskTable(uint32_t memoryAddress);

/**
 * @brief Returns a reference to the u8g2 display structure.
 *
 * Provides a pointer to the global u8g2 structure that holds configuration and
 * buffer information used in display operations.
 *
 * @return A pointer to the u8g2 structure.
 */
u8g2_t* display_getU8g2Ref();

/**
 * @brief Scrolls up the display content.
 *
 * Moves the contents of the u8g2 buffer upward by the specified number of
 * bytes, blanking out the bottom area.
 *
 * @param blankBytes The number of bytes to scroll up and clear.
 */
void display_scrollup(uint16_t blankBytes);

/**
 * @brief Retrieves the display buffer address.
 *
 * Returns the memory address of the display buffer.
 *
 * @return The display buffer memory address.
 */
uint32_t display_getAddress();

/**
 * @brief Retrieves the display command address.
 *
 * Returns the command address used for sending instructions to the display
 * hardware. Use to send commands to the display terminal application.
 *
 * @return The display command address.
 */
uint32_t display_getCommandAddress();

/**
 * @brief Retrieves the high-resolution translation table address. Used to
 * speedup the upscale of the high resolution display.
 *
 * Returns the memory address of the translation table required for
 * high-resolution display operations.
 *
 * @return The high-resolution translation table memory address.
 */
uint32_t display_getHighresTranstableAddress();

#endif  // DISPLAY_H
