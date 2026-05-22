/**
 * File: chandler.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2023 - April 2025
 * Copyright: 2023 2025 - GOODDATA LABS SL
 * Description: Header file for the Command Handler C program.
 */

#ifndef CHANDLER_H
#define CHANDLER_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "constants.h"
#include "debug.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "tprotocol.h"

#define CHANDLER_ADDRESS_HIGH_BIT 0x8000  // High bit of the address
#define CHANDLER_PARAMETERS_MAX_SIZE 20  // Max size of the parameters for debug

// All offsets are relative to __rom_in_ram_start__, which mirrors
// ROM4_ADDR ($FA0000) on the m68k side. Layout (single source of truth,
// must match target/atarist/src/main.s):
//
//   $FA0000  CARTRIDGE             m68k header + code (max 16 KB)
//                                  Now includes the unrolled MOVEM
//                                  block (fbdrv) at offset $2000
//                                  (Story 1.2.6). Cart budget bumped
//                                  from 8 KB to 16 KB.
//   $FA4000  CMD_MAGIC_SENTINEL    4 B  (m68k polls here for NOP/RESET/
//                                        BOOT_GEM/TERMINAL)
//   $FA4004  RANDOM_TOKEN          4 B  (chandler echoes the request token)
//   $FA4008  RANDOM_TOKEN_SEED     4 B
//   $FA400C  FB_FRAME_COUNTER      4 B  (RP-incremented dirty-frame marker;
//                                        m68k VBL loop skips the cart->ST
//                                        blit when this is unchanged since
//                                        the previous iteration)
//   $FA4010  SHARED_VARIABLES    240 B  (60 indexed 4-byte slots)
//   $FA4100  APP_FREE           ~16.5 KB free arena, ends at FRAMEBUFFER
//   $FA8300  FRAMEBUFFER          32 KB  (320x200 4bpp low-res; flush to
//                                         the top of the 64 KB region so
//                                         an overrun walks off the end
//                                         into unused RP RAM)
//   $FAFFFF  end of region
#define CHANDLER_CARTRIDGE_CODE_SIZE       0x4000  /* 16 KB cartridge budget */
#define CHANDLER_SHARED_BLOCK_OFFSET       CHANDLER_CARTRIDGE_CODE_SIZE
#define CHANDLER_CMD_SENTINEL_OFFSET       CHANDLER_SHARED_BLOCK_OFFSET
#define CHANDLER_RANDOM_TOKEN_OFFSET       (CHANDLER_CMD_SENTINEL_OFFSET + 4)
#define CHANDLER_RANDOM_TOKEN_SEED_OFFSET  (CHANDLER_RANDOM_TOKEN_OFFSET + 4)
/* Framebuffer dirty-frame counter. The RP writes a monotonically
 * increasing 32-bit value here as the LAST step of fb_render_frame()
 * (after a memory barrier to make sure all FB writes have committed).
 * The m68k userfw VBL loop reads this each frame and skips the cart->ST
 * blit + video-base flip when the value is unchanged since the previous
 * iteration. This rolls "tearing fence" + "skip work when idle" into a
 * single monotonic counter. chandler_init zeroes it at boot. */
#define CHANDLER_FB_FRAME_COUNTER_OFFSET   (CHANDLER_RANDOM_TOKEN_SEED_OFFSET + 4)
#define CHANDLER_SHARED_VARIABLES_OFFSET   (CHANDLER_FB_FRAME_COUNTER_OFFSET + 4)
#define CHANDLER_SHARED_VARIABLES_SLOTS    60  /* 240 bytes total */
#define CHANDLER_APP_FREE_OFFSET                                               \
  (CHANDLER_SHARED_VARIABLES_OFFSET + (CHANDLER_SHARED_VARIABLES_SLOTS * 4))

/* Framebuffer is now sized for low-res 4 bpp (320 x 200 = 32000 bytes).
 * It sits flush against the top of the 64 KB region: end = $FB0000 exactly,
 * start = $FB0000 - 32000 = $FA8300. APP_FREE's upper bound is implicitly
 * the framebuffer base. */
#define CHANDLER_FRAMEBUFFER_SIZE  32000
#define CHANDLER_FRAMEBUFFER_OFFSET (0x10000 - CHANDLER_FRAMEBUFFER_SIZE)
#define CHANDLER_REGION_END        0x10000  /* 64 KB shared region top */

// Index for the common shared variables
#define CHANDLER_HARDWARE_TYPE 0
#define CHANDLER_SVERSION 1
#define CHANDLER_BUFFER_TYPE 2

// Maximum number of command callbacks that may be registered with
// chandler_addCB. Pick a small bound so a buggy app cannot leak
// unbounded malloc() allocations through repeated registration.
#define CHANDLER_MAX_CALLBACKS 16

// Callback function type
typedef void (*CommandCallback)(TransmissionProtocol *protocol,
                                uint16_t *payloadPtr);

// Node for linked list of callbacks
typedef struct CommandCallbackNode {
  CommandCallback cb;
  struct CommandCallbackNode *next;
} CommandCallbackNode;

// Function Prototypes
void chandler_init();
void __not_in_flash_func(chandler_loop)();

void __not_in_flash_func(chandler_addCB)(CommandCallback cb);

#endif  // CHANDLER_H
