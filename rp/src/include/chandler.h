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
//   $FA0000  CARTRIDGE             m68k header + code (max 8 KB)
//   $FA2000  CMD_MAGIC_SENTINEL    4 B  (m68k polls here for NOP/RESET/
//                                        BOOT_GEM/TERMINAL)
//   $FA2004  RANDOM_TOKEN          4 B  (chandler echoes the request token)
//   $FA2008  RANDOM_TOKEN_SEED     4 B
//   $FA200C  reserved              4 B
//   $FA2010  SHARED_VARIABLES    240 B  (60 indexed 4-byte slots)
//   $FA2100  APP_BUFFERS              ~48 KB arena
//                                      The first 512 bytes (until $FA2300)
//                                      are written by display_setupU8g2()
//                                      with the high-res mask table the
//                                      cartridge uses to render the
//                                      framebuffer in 640x400 mode. Apps
//                                      that don't use the high-res path
//                                      can reclaim those 512 bytes; apps
//                                      that do must skip the first
//                                      CHANDLER_HIGHRES_TRANSTABLE_SIZE
//                                      bytes of APP_BUFFERS.
//   $FA2300  APP_FREE                 free for app-specific use until the
//                                      framebuffer (~48 KB)
//   $FAE0C0  FRAMEBUFFER         8000 B  (320x200 mono; sits at the top
//                                         so an overrun walks off the end
//                                         of the 64 KB region and into
//                                         unused RP RAM)
//   $FAFFFF  end of region
#define CHANDLER_CARTRIDGE_CODE_SIZE       0x2000  /* 8 KB cartridge budget */
#define CHANDLER_SHARED_BLOCK_OFFSET       CHANDLER_CARTRIDGE_CODE_SIZE
#define CHANDLER_CMD_SENTINEL_OFFSET       CHANDLER_SHARED_BLOCK_OFFSET
#define CHANDLER_RANDOM_TOKEN_OFFSET       (CHANDLER_CMD_SENTINEL_OFFSET + 4)
#define CHANDLER_RANDOM_TOKEN_SEED_OFFSET  (CHANDLER_RANDOM_TOKEN_OFFSET + 4)
/* 4-byte slot reserved for future framework use. chandler_init zeroes
 * it at boot; apps must not write here. */
#define CHANDLER_RESERVED_OFFSET           (CHANDLER_RANDOM_TOKEN_SEED_OFFSET + 4)
#define CHANDLER_SHARED_VARIABLES_OFFSET   (CHANDLER_RESERVED_OFFSET + 4)
#define CHANDLER_SHARED_VARIABLES_SLOTS    60  /* 240 bytes total */
#define CHANDLER_APP_BUFFERS_OFFSET                                            \
  (CHANDLER_SHARED_VARIABLES_OFFSET + (CHANDLER_SHARED_VARIABLES_SLOTS * 4))

/* High-res mask table written by display_generateMaskTable() at the start
 * of APP_BUFFERS. 256 entries x 16 bits = 512 bytes. Apps using the
 * high-res rendering path must not reuse this region. */
#define CHANDLER_HIGHRES_TRANSTABLE_OFFSET CHANDLER_APP_BUFFERS_OFFSET
#define CHANDLER_HIGHRES_TRANSTABLE_SIZE   0x200  /* 512 bytes */
#define CHANDLER_APP_FREE_OFFSET                                               \
  (CHANDLER_HIGHRES_TRANSTABLE_OFFSET + CHANDLER_HIGHRES_TRANSTABLE_SIZE)

#define CHANDLER_FRAMEBUFFER_SIZE  0x1F40  /* 8000 bytes (320x200 mono) */
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
