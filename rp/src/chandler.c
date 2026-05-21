/**
 * File: chandler.c
 * Author: Diego Parrilla Santamaría
 * Date: November 2023-April 2025
 * Copyright: 2023 2025 - GOODDATA LABS SL
 * Description: Command Handler for the SidecarTridge protocol
 */

#include "chandler.h"

#include <stdlib.h>
#include <string.h>

#include "commemul.h"

static TransmissionProtocol pendingProtocol;
static bool protocolPending = false;

static uint32_t incrementalCmdCount = 0;

// Address of the random-token reply slot (chandler_loop publishes the
// 64-bit { incrementalCmdCount | randomToken } value here so the m68k's
// send_sync poll wakes up).
static uint32_t memoryRandomTokenAddress = 0;

// Head of the callback list and current count (capped at
// CHANDLER_MAX_CALLBACKS).
static CommandCallbackNode *callbackListHead = NULL;
static unsigned int callbackListCount = 0;

static inline void __not_in_flash_func(chandler_clear_pending_protocol)(void) {
  pendingProtocol.command_id = 0;
  pendingProtocol.payload_size = 0;
  pendingProtocol.bytes_read = 0;
  pendingProtocol.final_checksum = 0;
  protocolPending = false;
}

static inline bool __not_in_flash_func(chandler_protocol_matches_pending)(
    const TransmissionProtocol *protocol, uint16_t size) {
  if (!protocolPending) {
    return false;
  }

  if ((pendingProtocol.command_id != protocol->command_id) ||
      (pendingProtocol.payload_size != protocol->payload_size) ||
      (pendingProtocol.final_checksum != protocol->final_checksum)) {
    return false;
  }

  return memcmp(pendingProtocol.payload, protocol->payload, size) == 0;
}

void __not_in_flash_func(chandler_init)() {
  DPRINTF("Initializing Command Handler...\n");

  uint32_t shared_base = (unsigned int)&__rom_in_ram_start__;
  memoryRandomTokenAddress = shared_base + CHANDLER_RANDOM_TOKEN_OFFSET;

  // Seed the random-token slots with non-zero values before the first
  // m68k command runs. send_sync_command_to_sidecart reads
  // RANDOM_TOKEN_SEED_ADDR to derive its request token; on a cold boot
  // that slot would otherwise be whatever the RP RAM happened to contain.
  uint64_t boot_us = time_us_64();
  uint32_t seed = (uint32_t)(boot_us ^ (boot_us >> 32));
  if (seed == 0) {
    seed = 0xA1F0C0DEu;
  }
  TPROTO_SET_RANDOM_TOKEN(memoryRandomTokenAddress, seed);
  TPROTO_SET_RANDOM_TOKEN(shared_base + CHANDLER_RANDOM_TOKEN_SEED_OFFSET,
                          seed ^ 0xDEADBEEFu);

  // Zero the reserved 4-byte slot so apps can rely on a known initial
  // value if the framework later claims it for something concrete.
  *((volatile uint32_t *)(shared_base + CHANDLER_RESERVED_OFFSET)) = 0;

  chandler_clear_pending_protocol();
}

/**
 * @brief Register a callback
 *
 * Adds the callback to the linked list of callbacks.
 *
 * @param cb The callback function to register.
 */
void __not_in_flash_func(chandler_addCB)(CommandCallback cb) {
  if (!cb) return;
  if (callbackListCount >= CHANDLER_MAX_CALLBACKS) {
    DPRINTF("chandler_addCB: callback list full (cap=%u), refusing %p\n",
            (unsigned)CHANDLER_MAX_CALLBACKS, (void *)cb);
    return;
  }
  // Reject duplicates so accidental double-registration doesn't double-
  // dispatch every command (and doesn't waste a slot).
  for (CommandCallbackNode *cur = callbackListHead; cur; cur = cur->next) {
    if (cur->cb == cb) {
      DPRINTF("chandler_addCB: %p already registered, ignoring\n",
              (void *)cb);
      return;
    }
  }
  CommandCallbackNode *node = malloc(sizeof(*node));
  if (!node) return;
  node->cb = cb;
  node->next = NULL;
  if (!callbackListHead) {
    callbackListHead = node;
  } else {
    CommandCallbackNode *cur = callbackListHead;
    while (cur->next) cur = cur->next;
    cur->next = node;
  }
  callbackListCount++;
}

static inline void __not_in_flash_func(handle_protocol_command)(
    const TransmissionProtocol *protocol) {
  uint16_t size = tprotocol_clamp_payload_size(protocol->payload_size);

  if (protocolPending) {
    if (!chandler_protocol_matches_pending(protocol, size)) {
      DPRINTF("Ignoring protocol %04x (%u bytes) while %04x is pending\n",
              protocol->command_id, protocol->payload_size,
              pendingProtocol.command_id);
    }
    return;
  }

  tprotocol_copy_safely(&pendingProtocol, protocol);
  protocolPending = true;
}

static inline void __not_in_flash_func(handle_protocol_checksum_error)(
    const TransmissionProtocol *protocol) {
  DPRINTF(
      "Checksum error detected (CommandID=%x, Size=%x, Bytes Read=%x, "
      "Chksum=%x, RTOKEN=%x)\n",
      protocol->command_id, protocol->payload_size, protocol->bytes_read,
      protocol->final_checksum, TPROTO_GET_RANDOM_TOKEN(protocol->payload));
}

static inline void __not_in_flash_func(chandler_consume_rom3_sample)(
    uint16_t sample) {
  uint16_t addr_lsb = (uint16_t)(sample ^ CHANDLER_ADDRESS_HIGH_BIT);

  tprotocol_parse(addr_lsb, handle_protocol_command,
                  handle_protocol_checksum_error);
}

// Invoke this function to process the commands from the active loop in the
// main function
void __not_in_flash_func(chandler_loop)() {
  commemul_poll(chandler_consume_rom3_sample);

  if (!protocolPending) {
    // No command to process
    return;
  }

  // Shared by all commands
  // Read the random token from the command and increment the payload
  // pointer to the first parameter available in the payload
  uint32_t randomToken = TPROTO_GET_RANDOM_TOKEN(pendingProtocol.payload);
  uint16_t *payloadPtr = (uint16_t *)pendingProtocol.payload;
  uint16_t commandId = pendingProtocol.command_id;
  if ((commandId == 0) && (pendingProtocol.payload_size == 0) &&
      (pendingProtocol.final_checksum == 0)) {
    // Invalid command, clear the pending slot and return
    DPRINTF("Invalid command received. Ignoring.\n");
    chandler_clear_pending_protocol();
    return;
  }

  // Jump the random token
  TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);

  for (CommandCallbackNode *cur = callbackListHead; cur; cur = cur->next) {
    if (cur->cb) cur->cb(&pendingProtocol, payloadPtr);
  }

  incrementalCmdCount++;
  // 64-bit write: low 32b -> RANDOM_TOKEN (echoes request token), high 32b
  // -> RANDOM_TOKEN_SEED (incrementalCmdCount). SEED MUST differ from the
  // request token: the m68k waiter uses "SEED advanced" to distinguish a
  // real reply from open-bus reads when no firmware is responding.
  TPROTO_SET_RANDOM_TOKEN64(
      memoryRandomTokenAddress,
      (((uint64_t)incrementalCmdCount) << 32) | randomToken);

  chandler_clear_pending_protocol();
}
