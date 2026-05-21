/**
 * File: term.c
 * Author: Diego Parrilla Santamaría
 * Date: January 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Online terminal
 */

#include "term.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aconfig.h"
#include "chandler.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "display_term.h"
#include "gconfig.h"
#include "memfunc.h"
#include "network.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "tprotocol.h"

// Producer (term_command_cb, called from chandler_loop) and consumer
// (term_loop) both run sequentially on core 0, so the double-buffer swap
// doesn't need volatile or an IRQ-disable critical section.
static TransmissionProtocol protocolBuffers[2];
static uint8_t protocolReadIndex = 0;
static uint8_t protocolWriteIndex = 1;
static bool protocolBufferReady = false;
static uint32_t protocolOverwriteCount = 0;

#define TERM_NETWORK_INFO_VALUE_SIZE 64
#define TERM_MENU_LIVE_LINE_MAX 128

static void term_getConfigValueOrNA(const char *key, char *buffer,
                                    size_t bufferSize) {
  if (bufferSize == 0) {
    return;
  }

  buffer[0] = '\0';
  SettingsConfigEntry *entry = settings_find_entry(gconfig_getContext(), key);
  if ((entry != NULL) && (entry->value != NULL) && (entry->value[0] != '\0')) {
    snprintf(buffer, bufferSize, "%s", entry->value);
    return;
  }

  snprintf(buffer, bufferSize, "N/A");
}

#if defined(CYW43_WL_GPIO_LED_PIN)
static void term_getIpStringOrNA(const ip_addr_t *address, char *buffer,
                                 size_t bufferSize) {
  if (bufferSize == 0) {
    return;
  }

  if ((address == NULL) || ip_addr_isany(address)) {
    snprintf(buffer, bufferSize, "N/A");
    return;
  }

  const char *ipString = ipaddr_ntoa(address);
  if ((ipString != NULL) && (ipString[0] != '\0')) {
    snprintf(buffer, bufferSize, "%s", ipString);
  } else {
    snprintf(buffer, bufferSize, "N/A");
  }
}

static void term_getConfiguredDns(char *dns1, size_t dns1Size, char *dns2,
                                  size_t dns2Size) {
  if (dns1Size > 0) {
    snprintf(dns1, dns1Size, "N/A");
  }
  if (dns2Size > 0) {
    snprintf(dns2, dns2Size, "N/A");
  }

  SettingsConfigEntry *entry =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_DNS);
  if ((entry == NULL) || (entry->value == NULL) || (entry->value[0] == '\0')) {
    return;
  }

  char dnsValue[(TERM_NETWORK_INFO_VALUE_SIZE * 2) + 2] = {0};
  snprintf(dnsValue, sizeof(dnsValue), "%s", entry->value);

  char *dnsSecond = strchr(dnsValue, ',');
  if (dnsSecond != NULL) {
    *dnsSecond = '\0';
    dnsSecond++;
    while (*dnsSecond == ' ') {
      dnsSecond++;
    }
  }

  if ((dns1Size > 0) && (dnsValue[0] != '\0')) {
    snprintf(dns1, dns1Size, "%s", dnsValue);
  }

  if ((dns2Size > 0) && (dnsSecond != NULL) && (dnsSecond[0] != '\0')) {
    snprintf(dns2, dns2Size, "%s", dnsSecond);
  }
}
#endif

// Command handlers
static void cmdClear(const char *arg);
static void cmdExit(const char *arg);
static void cmdHelp(const char *arg);
static void cmdUnknown(const char *arg);

// Command table
static const Command *commands;

// Number of commands in the table
static size_t numCommands = 0;

// Setter for commands and numCommands
void term_setCommands(const Command *cmds, size_t count) {
  commands = cmds;
  numCommands = count;
}

/**
 * @brief chandler callback that publishes a fully-parsed protocol command
 *        into the terminal's double-buffer for term_loop() to consume.
 *
 * Called from chandler_loop() on core 0 (or wherever the application drains
 * commemul). Copies the command into the inactive slot and atomically swaps
 * read/write roles. payloadPtr is provided by chandler (already advanced past
 * the random token) but term doesn't use it directly — it re-derives the
 * pointer from the published snapshot inside term_loop().
 */
void __not_in_flash_func(term_command_cb)(TransmissionProtocol *protocol,
                                          uint16_t *payloadPtr) {
  (void)payloadPtr;
  uint8_t writeIndex = protocolWriteIndex;
  TransmissionProtocol *writeBuffer = &protocolBuffers[writeIndex];

  // Copy the content of protocol to the active write buffer.
  writeBuffer->command_id = protocol->command_id;
  writeBuffer->payload_size = protocol->payload_size;
  writeBuffer->bytes_read = protocol->bytes_read;
  writeBuffer->final_checksum = protocol->final_checksum;

  // Sanity check: clamp payload_size to avoid overflow
  uint16_t size = protocol->payload_size;
  if (size > MAX_PROTOCOL_PAYLOAD_SIZE) {
    size = MAX_PROTOCOL_PAYLOAD_SIZE;
  }

  // Copy only used payload bytes
  memcpy(writeBuffer->payload, protocol->payload, size);

  if (protocolBufferReady) {
    protocolOverwriteCount++;
  }

  // Publish atomically by swapping read/write roles.
  uint8_t readIndex = protocolReadIndex;
  protocolReadIndex = writeIndex;
  protocolWriteIndex = readIndex;
  protocolBufferReady = true;
}

static char screen[TERM_SCREEN_SIZE];
static uint8_t cursorX = 0;
static uint8_t cursorY = 0;

static uint8_t menuRowSsid = 0;
static uint8_t menuRowSelect = 0;
static uint8_t menuRowSd = 0;
static uint8_t menuPromptRow = 0;
static uint8_t menuPromptCol = 0;
static bool menuRowsValid = false;
static bool menuPromptValid = false;

// Store previous cursor position for block removal
static uint8_t prevCursorX = 0;
static uint8_t prevCursorY = 0;

// Buffer to keep track of chars entered between newlines
static char inputBuffer[TERM_INPUT_BUFFER_SIZE];
static size_t inputLength = 0;

// Getter method for inputBuffer
char *term_getInputBuffer(void) { return inputBuffer; }

// Clears entire screen buffer and resets cursor
void term_clearScreen(void) {
  memset(screen, 0, TERM_SCREEN_SIZE);
  cursorX = 0;
  cursorY = 0;
  menuRowsValid = false;
  menuPromptValid = false;
  display_termClear();
}

// Clears the input buffer
void term_clearInputBuffer(void) {
  memset(inputBuffer, 0, TERM_INPUT_BUFFER_SIZE);
  inputLength = 0;
}

// Custom scrollup function to scroll except for the last row
void termScrollupBuffer(uint16_t blankBytes) {
  // blank bytes is the number of bytes to blank out at the bottom of the
  // screen

  unsigned char *u8g2Buffer = u8g2_GetBufferPtr(display_getU8g2Ref());
  memmove(u8g2Buffer, u8g2Buffer + blankBytes,
          DISPLAY_BUFFER_SIZE - blankBytes -
              TERM_SCREEN_SIZE_X * DISPLAY_TERM_CHAR_HEIGHT);
  memset(u8g2Buffer + DISPLAY_BUFFER_SIZE - blankBytes -
             TERM_SCREEN_SIZE_X * DISPLAY_TERM_CHAR_HEIGHT,
         0, blankBytes);
}

// Scrolls the screen up by one row
static void termScrollUp(void) {
  memmove(screen, screen + TERM_SCREEN_SIZE_X,
          TERM_SCREEN_SIZE - TERM_SCREEN_SIZE_X);
  memset(screen + TERM_SCREEN_SIZE - TERM_SCREEN_SIZE_X, 0, TERM_SCREEN_SIZE_X);
  termScrollupBuffer(TERM_DISPLAY_ROW_BYTES);
}

// Prints a character to the screen, handles scrolling
static void termPutChar(char chr) {
  screen[cursorY * TERM_SCREEN_SIZE_X + cursorX] = chr;
  display_termChar(cursorX, cursorY, chr);
  cursorX++;
  if (cursorX >= TERM_SCREEN_SIZE_X) {
    cursorX = 0;
    cursorY++;
    if (cursorY >= TERM_SCREEN_SIZE_Y) {
      termScrollUp();
      cursorY = TERM_SCREEN_SIZE_Y - 1;
    }
  }
}

// Renders a single character, with special handling for newline and carriage
// return
static void termRenderChar(char chr) {
  // First, remove the old block by restoring the character
  display_termChar(prevCursorX, prevCursorY, ' ');
  if (chr == '\n' || chr == '\r') {
    // Move to new line
    cursorX = 0;
    cursorY++;
    if (cursorY >= TERM_SCREEN_SIZE_Y) {
      termScrollUp();
      cursorY = TERM_SCREEN_SIZE_Y - 1;
    }
  } else if (chr != '\0') {
    termPutChar(chr);
  }

  // Draw a block at the new cursor position
  display_termCursor(cursorX, cursorY);
  // Update previous cursor coords
  prevCursorX = cursorX;
  prevCursorY = cursorY;
}

/**
 * @brief Processes a complete VT52 escape sequence.
 *
 * This function interprets the VT52 sequence stored in `seq` (with given
 * length) and performs the corresponding cursor movements. Modify the TODO
 * sections to implement additional features as needed.
 *
 * @param seq Pointer to the escape sequence buffer.
 * @param length The length of the escape sequence.
 */
static void vt52ProcessSequence(const char *seq, size_t length) {
  // Ensure we have at least an ESC and a command character.
  if (length < 2) return;

  char command = seq[1];
  switch (command) {
    case 'A':  // Move cursor up
      // TODO(diego): Improve behavior if needed.
      if (cursorY > 0) {
        cursorY--;
      }
      termRenderChar('\0');
      break;
    case 'B':  // Move cursor down
      if (cursorY < TERM_SCREEN_SIZE_Y - 1) {
        cursorY++;
      }
      termRenderChar('\0');
      break;
    case 'C':  // Move cursor right
      if (cursorX < TERM_SCREEN_SIZE_X - 1) {
        cursorX++;
      }
      termRenderChar('\0');
      break;
    case 'D':  // Move cursor left
      if (cursorX > 0) {
        cursorX--;
      }
      termRenderChar('\0');
      break;
    case 'E':  // Clear screen and place cursor at top left corner
      cursorX = 0;
      cursorY = 0;
      termRenderChar('\0');
      for (int posY = 0; posY < TERM_SCREEN_SIZE_Y; posY++) {
        for (int posX = 0; posX < TERM_SCREEN_SIZE_X; posX++) {
          screen[posY * TERM_SCREEN_SIZE_X + posX] = 0;
          display_termChar(posX, posY, ' ');
        }
      }
      break;
    case 'H':  // Cursor home
      cursorX = 0;
      cursorY = 0;
      termRenderChar('\0');
      break;
    case 'J':  // Erases from the current cursor position to the end of the
               // screen
      for (int posY = cursorY; posY < TERM_SCREEN_SIZE_Y; posY++) {
        for (int posX = cursorX; posX < TERM_SCREEN_SIZE_X; posX++) {
          screen[posY * TERM_SCREEN_SIZE_X + posX] = 0;
          display_termChar(posX, posY, ' ');
        }
      }
      break;
    case 'K':  // Clear to end of line
      for (int posX = cursorX; posX < TERM_SCREEN_SIZE_X; posX++) {
        screen[cursorY * TERM_SCREEN_SIZE_X + posX] = 0;
        display_termChar(posX, cursorY, ' ');
      }
      break;
    case 'Y':  // Direct cursor addressing: ESC Y <row> <col>
      if (length == 4) {
        int row = seq[2] - TERM_POS_Y;
        int col = seq[3] - TERM_POS_X;
        if (row >= 0 && row < TERM_SCREEN_SIZE_Y && col >= 0 &&
            col < TERM_SCREEN_SIZE_X) {
          cursorY = row;
          cursorX = col;
        }
        termRenderChar('\0');
      }
      break;
    default:
      // Unrecognized sequence. Optionally, print or ignore.
      // For now, we'll ignore it.
      break;
  }
}

void term_printString(const char *str) {
  enum { STATE_NORMAL, STATE_ESC } state = STATE_NORMAL;
  char escBuffer[TERM_ESC_BUFFLINE_SIZE];
  size_t escLen = 0;

  while (*str) {
    char chr = *str;
    if (state == STATE_NORMAL) {
      if (chr == TERM_ESC_CHAR) {  // ESC character detected
        state = STATE_ESC;
        escLen = 0;
        escBuffer[escLen++] = chr;
      } else {
        termRenderChar(chr);
      }
    } else {  // STATE_ESC: we're accumulating an escape sequence
      escBuffer[escLen++] = chr;
      // Check for sequence completion:
      // Most VT52 sequences are two characters (ESC + command)...
      if (escLen == 2) {
        if (escBuffer[1] == 'Y') {
          // ESC Y requires two more characters (for row and col)
          // Do nothing now—wait until esc_len == 4.
        } else {
          // Sequence complete (ESC + single command)
          vt52ProcessSequence(escBuffer, escLen);
          state = STATE_NORMAL;
        }
      } else if (escBuffer[1] == 'Y' && escLen == 4) {
        // ESC Y <row> <col> sequence complete
        vt52ProcessSequence(escBuffer, escLen);
        state = STATE_NORMAL;
      }
      // In case the buffer gets too long, flush it as normal text
      if (escLen >= sizeof(escBuffer)) {
        for (size_t i = 0; i < escLen; i++) {
          termRenderChar(escBuffer[i]);
        }
        state = STATE_NORMAL;
      }
    }
    str++;
  }
  // If the string ends while still in ESC state, flush the buffered
  // characters as normal text.
  if (state == STATE_ESC) {
    for (size_t i = 0; i < escLen; i++) {
      termRenderChar(escBuffer[i]);
    }
  }
  display_termRefresh();
}

// Called whenever a character is entered by the user
// This is the single point of entry for user input
static void termInputChar(char chr) {
  // Check for backspace
  if (chr == '\b') {
    display_termChar(prevCursorX, prevCursorY, ' ');

    // If we have chars in input_buffer, remove last char
    if (inputLength > 0) {
      inputLength--;
      inputBuffer[inputLength] = '\0';  // Null-terminate the string
      // Also reflect on screen
      // same as term_backspace
      if (cursorX == 0) {
        if (cursorY > 0) {
          cursorY--;
          cursorX = TERM_SCREEN_SIZE_X - 1;
        } else {
          // already top-left corner
          return;
        }
      } else {
        cursorX--;
      }
      screen[cursorY * TERM_SCREEN_SIZE_X + cursorX] = 0;
      display_termChar(cursorX, cursorY, ' ');
    }

    display_termCursor(cursorX, cursorY);
    prevCursorX = cursorX;
    prevCursorY = cursorY;
    display_termRefresh();
    return;
  }

  // If it's newline or carriage return, finalize the line
  if (chr == '\n' || chr == '\r') {
    // Render newline on screen
    termRenderChar('\n');

    // Process input_buffer
    // Split the input into command and argument
    char command[TERM_INPUT_BUFFER_SIZE] = {0};
    char arg[TERM_INPUT_BUFFER_SIZE] = {0};
    sscanf(inputBuffer, "%63s %63[^\n]", command,
           arg);  // Split at the first space

    bool commandFound = false;
    for (size_t i = 0; i < numCommands; i++) {
      if (strcmp(command, commands[i].command) == 0) {
        commands[i].handler(arg);  // Pass the argument to the handler
        commandFound = true;
      }
    }
    if ((!commandFound) && (strlen(command) > 0)) {
      // The custom unknown command manager is called when the command is empty
      // in the command table. This is useful to manage custom entries.
      for (size_t i = 0; i < numCommands; i++) {
        if (strlen(commands[i].command) == 0) {
          commands[i].handler(inputBuffer);  // Pass the argument to the handler
        }
      }
    }

    // Reset input buffer
    memset(inputBuffer, 0, TERM_INPUT_BUFFER_SIZE);
    inputLength = 0;

    term_printString("> ");
    display_termRefresh();
    return;
  }

  // If it's a normal character
  // Add it to input_buffer if there's space
  if (inputLength < TERM_INPUT_BUFFER_SIZE - 1) {
    inputBuffer[inputLength++] = chr;
    // Render char on screen
    termRenderChar(chr);

    // show block cursor

    display_termRefresh();
  } else {
    // Buffer full, ignore or beep?
  }
}

void term_init(void) {
  // Random-token publish, seed init, and shared-variable bookkeeping moved
  // to chandler (chandler_init / chandler_loop). term_init only handles
  // terminal display state now.

  // Initialize the welcome messages
  term_clearScreen();
  term_printString("Welcome to the terminal!\n");
  term_printString("Press ESC to enter the terminal.\n");
  term_printString("or any SHIFT key to boot the desktop.\n");

  // Example 1: Move the cursor up one line.
  // VT52 sequence: ESC A (moves cursor up)
  // The escape sequence "\x1BA" will move the cursor up one line.
  // term_printString("\x1B" "A");
  // After moving up, print text that overwrites part of the previous line.
  // term_printString("Line 2 (modified by ESC A)\n");

  // Example 2: Move the cursor right one character.
  // VT52 sequence: ESC C (moves cursor right)
  // term_printString("\x1B" "C");
  // term_printString(" <-- Moved right with ESC C\n");

  // Example 3: Direct cursor addressing.
  // VT52 direct addressing uses ESC Y <row> <col>, where:
  //   row_char = row + 0x20, col_char = col + 0x20.
  // For instance, to move the cursor to row 0, column 10:
  //   row: 0 -> 0x20 (' ')
  //   col: 10 -> 0x20 + 10 = 0x2A ('*')
  // term_printString("\x1B" "Y" "\x20" "\x2A");
  // term_printString("Text at row 0, column 10 via ESC Y\n");

  // term_printString("\x1B" "Y" "\x2A" "\x20");

  display_refresh();
}

// Invoke this function to process the commands from the active loop in the
// main function
void __not_in_flash_func(term_loop)() {
  TransmissionProtocol protocolSnapshot = {0};
  bool protocolReady = false;
  uint32_t overwriteCountSnapshot = 0;

  // Snapshot the latest published slot. Producer and consumer are both
  // on core 0 and never re-enter, so no critical section is needed.
  if (protocolBufferReady) {
    protocolSnapshot = protocolBuffers[protocolReadIndex];
    protocolBufferReady = false;
    protocolReady = true;
  }
  overwriteCountSnapshot = protocolOverwriteCount;

  if (protocolReady) {
    // Shared by all commands
    // Read the random token from the command and increment the payload
    // pointer to the first parameter available in the payload
    uint32_t randomToken = TPROTO_GET_RANDOM_TOKEN(protocolSnapshot.payload);
    uint16_t *payloadPtr = ((uint16_t *)(protocolSnapshot).payload);
    uint16_t commandId = protocolSnapshot.command_id;
    DPRINTF(
        "Command ID: %d. Size: %d. Random token: 0x%08X, Checksum: 0x%04X, "
        "Overwrites: %lu\n",
        protocolSnapshot.command_id, protocolSnapshot.payload_size, randomToken,
        protocolSnapshot.final_checksum, (unsigned long)overwriteCountSnapshot);

#if defined(_DEBUG) && (_DEBUG != 0)
    // Jump the random token
    TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);

    // Read the payload parameters
    uint16_t payloadSizeTmp = 4;
    if ((protocolSnapshot.payload_size > payloadSizeTmp) &&
        (protocolSnapshot.payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D3: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
    payloadSizeTmp += 4;
    if ((protocolSnapshot.payload_size > payloadSizeTmp) &&
        (protocolSnapshot.payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D4: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
    payloadSizeTmp += 4;
    if ((protocolSnapshot.payload_size > payloadSizeTmp) &&
        (protocolSnapshot.payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D5: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
    payloadSizeTmp += 4;
    if ((protocolSnapshot.payload_size > payloadSizeTmp) &&
        (protocolSnapshot.payload_size <= TERM_PARAMETERS_MAX_SIZE)) {
      DPRINTF("Payload D6: 0x%04X\n", TPROTO_GET_PAYLOAD_PARAM32(payloadPtr));
      TPROTO_NEXT32_PAYLOAD_PTR(payloadPtr);
    }
#endif

    // Handle the command
    switch (protocolSnapshot.command_id) {
      case APP_TERMINAL_START: {
        display_termStart(DISPLAY_TILES_WIDTH, DISPLAY_TILES_HEIGHT);
        term_clearScreen();
        term_printString("Type 'help' for available commands.\n");
        termInputChar('\n');
        SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_TERM);
        DPRINTF("Send command to display: DISPLAY_COMMAND_TERM\n");
      } break;
      case APP_TERMINAL_KEYSTROKE: {
        uint16_t *payload = ((uint16_t *)(protocolSnapshot).payload);
        // Jump the random token
        TPROTO_NEXT32_PAYLOAD_PTR(payload);
        // Extract the 32 bit payload
        uint32_t payload32 = TPROTO_GET_PAYLOAD_PARAM32(payload);
        // Extract the ascii code from the payload lower 8 bits
        char keystroke = (char)(payload32 & TERM_KEYBOARD_KEY_MASK);
        // Get the shift key status from the higher byte of the payload
        uint8_t shiftKey =
            (payload32 & TERM_KEYBOARD_SHIFT_MASK) >> TERM_KEYBOARD_SHIFT_SHIFT;
        // Get the keyboard scan code from the bits 16 to 23 of the payload
        uint8_t scanCode =
            (payload32 & TERM_KEYBOARD_SCAN_MASK) >> TERM_KEYBOARD_SCAN_SHIFT;
        if (keystroke >= TERM_KEYBOARD_KEY_START &&
            keystroke <= TERM_KEYBOARD_KEY_END) {
          // Print the keystroke and the shift key status
          DPRINTF("Keystroke: %c. Shift key: %d, Scan code: %d\n", keystroke,
                  shiftKey, scanCode);
        } else {
          // Print the keystroke and the shift key status
          DPRINTF("Keystroke: %d. Shift key: %d, Scan code: %d\n", keystroke,
                  shiftKey, scanCode);
        }
        termInputChar(keystroke);
        break;
      }
      default:
        // Unknown command
        DPRINTF("Unknown command\n");
        break;
    }
    // Random-token publish is owned by chandler_loop; nothing more to do here.
    (void)randomToken;
  }
}

// Command handlers
void term_printNetworkInfo(void) {
  char hostName[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char ipAddress[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char gateway[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char dns1[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char dns2[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char netmask[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char ssid[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char bssid[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char authMode[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char signalDb[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char wifiMode[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char wifiLink[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char ipMode[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char wifiMac[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char mcuArch[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char mcuId[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char selectState[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char sdStatus[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char sdSpace[TERM_NETWORK_INFO_VALUE_SIZE] = {0};

  term_getConfigValueOrNA(PARAM_HOSTNAME, hostName, sizeof(hostName));
  term_getConfigValueOrNA(PARAM_WIFI_IP, ipAddress, sizeof(ipAddress));
  term_getConfigValueOrNA(PARAM_WIFI_GATEWAY, gateway, sizeof(gateway));
  term_getConfigValueOrNA(PARAM_WIFI_NETMASK, netmask, sizeof(netmask));
#if defined(CYW43_WL_GPIO_LED_PIN)
  term_getConfiguredDns(dns1, sizeof(dns1), dns2, sizeof(dns2));
#else
  snprintf(dns1, sizeof(dns1), "N/A");
  snprintf(dns2, sizeof(dns2), "N/A");
#endif
  snprintf(ssid, sizeof(ssid), "N/A");
  snprintf(bssid, sizeof(bssid), "N/A");
  snprintf(authMode, sizeof(authMode), "N/A");
  snprintf(signalDb, sizeof(signalDb), "N/A");
  snprintf(wifiMode, sizeof(wifiMode), "N/A");
  snprintf(wifiLink, sizeof(wifiLink), "N/A");
  snprintf(ipMode, sizeof(ipMode), "N/A");
  snprintf(wifiMac, sizeof(wifiMac), "N/A");
  snprintf(mcuArch, sizeof(mcuArch), "N/A");
  snprintf(mcuId, sizeof(mcuId), "N/A");
  snprintf(selectState, sizeof(selectState), "%s",
           select_detectPush() ? "Pressed" : "Released");
  snprintf(sdStatus, sizeof(sdStatus), "Not mounted");
  snprintf(sdSpace, sizeof(sdSpace), "N/A");

  uint32_t sdTotalMb = 0;
  uint32_t sdFreeMb = 0;
  if (sdcard_getMountedInfo(&sdTotalMb, &sdFreeMb)) {
    snprintf(sdStatus, sizeof(sdStatus), "Mounted");
    snprintf(sdSpace, sizeof(sdSpace), "%lu/%lu MB free",
             (unsigned long)sdFreeMb, (unsigned long)sdTotalMb);
  } else if (sdcard_isMounted()) {
    snprintf(sdStatus, sizeof(sdStatus), "Error");
  }

  term_printString("Network status: ");

#if defined(CYW43_WL_GPIO_LED_PIN)
  const char *wifiModeValue = network_getWifiModeStr();
  if ((wifiModeValue != NULL) && (wifiModeValue[0] != '\0')) {
    snprintf(wifiMode, sizeof(wifiMode), "%s", wifiModeValue);
  }
  const char *wifiLinkValue = network_wifiConnStatusStr();
  if ((wifiLinkValue != NULL) && (wifiLinkValue[0] != '\0')) {
    snprintf(wifiLink, sizeof(wifiLink), "%s", wifiLinkValue);
  }

  SettingsConfigEntry *dhcpEntry =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_DHCP);
  if ((dhcpEntry != NULL) && (dhcpEntry->value != NULL) &&
      (dhcpEntry->value[0] != '\0')) {
    char dhcpChar = dhcpEntry->value[0];
    bool dhcpEnabled = (dhcpChar == 't') || (dhcpChar == 'T') ||
                       (dhcpChar == '1') || (dhcpChar == 'y') ||
                       (dhcpChar == 'Y');
    snprintf(ipMode, sizeof(ipMode), "%s", dhcpEnabled ? "DHCP" : "Static");
  }

  const char *wifiMacValue = network_getCyw43MacStr();
  if ((wifiMacValue != NULL) && (wifiMacValue[0] != '\0')) {
    snprintf(wifiMac, sizeof(wifiMac), "%s", wifiMacValue);
  }

  const char *mcuArchValue = network_getMcuArchStr();
  if ((mcuArchValue != NULL) && (mcuArchValue[0] != '\0')) {
    snprintf(mcuArch, sizeof(mcuArch), "%s", mcuArchValue);
  }
  const char *mcuIdValue = network_getMcuIdStr();
  if ((mcuIdValue != NULL) && (mcuIdValue[0] != '\0')) {
    snprintf(mcuId, sizeof(mcuId), "%s", mcuIdValue);
  }

  ip_addr_t currentIp = network_getCurrentIp();
  bool hasIp = !ip_addr_isany(&currentIp);
  term_printString(hasIp ? "Connected\n" : "Not connected\n");

  if (hasIp) {
    term_getIpStringOrNA(&currentIp, ipAddress, sizeof(ipAddress));

    wifi_network_info_t currentNetwork = network_getCurrentNetworkInfo();
    if (currentNetwork.ssid[0] != '\0') {
      snprintf(ssid, sizeof(ssid), "%s", currentNetwork.ssid);
      snprintf(authMode, sizeof(authMode), "%s",
               network_getAuthTypeString(currentNetwork.auth_mode));
    }
    if (currentNetwork.bssid[0] != '\0') {
      snprintf(bssid, sizeof(bssid), "%s", currentNetwork.bssid);
    }
    if ((currentNetwork.rssi <= 0) && (currentNetwork.rssi >= -120)) {
      snprintf(signalDb, sizeof(signalDb), "%d dBm", currentNetwork.rssi);
    }
  }

  struct netif *netIf = netif_default;

  if (netIf != NULL) {
    term_getIpStringOrNA(&(netIf->gw), gateway, sizeof(gateway));
    term_getIpStringOrNA(&(netIf->netmask), netmask, sizeof(netmask));

#if defined(LWIP_NETIF_HOSTNAME) && LWIP_NETIF_HOSTNAME
    const char *runtimeHostname = netif_get_hostname(netIf);
    if ((runtimeHostname != NULL) && (runtimeHostname[0] != '\0')) {
      snprintf(hostName, sizeof(hostName), "%s", runtimeHostname);
    }
#endif
  }

  term_getIpStringOrNA(dns_getserver(0), dns1, sizeof(dns1));
  term_getIpStringOrNA(dns_getserver(1), dns2, sizeof(dns2));
#else
  term_printString("Unavailable\n");
#endif

  menuRowsValid = false;

  TPRINTF("MCU type  : %s (%s)\n", mcuArch, mcuId);
  TPRINTF("Host name : %s\n", hostName);
  TPRINTF("WiFi      : %s (%s)\n", wifiMode, wifiLink);
  TPRINTF("IP        : %s (%s)\n", ipAddress, ipMode);
  TPRINTF("Netmask   : %s\n", netmask);
  TPRINTF("Gateway   : %s\n", gateway);
  TPRINTF("DNS       : %s, %s\n", dns1, dns2);
  TPRINTF("WiFi MAC  : %s\n", wifiMac);

  menuRowSsid = cursorY;
  TPRINTF("SSID      : %s (%s)\n", ssid, signalDb);

  TPRINTF("BSSID     : %s\n", bssid);
  TPRINTF("Auth mode : %s\n", authMode);

  term_printString("\n");
  menuRowSelect = cursorY;
  TPRINTF("SELECT  : %s\n", selectState);

  term_printString("\n");
  menuRowSd = cursorY;
  TPRINTF("SD card   : %s (%s)\n", sdStatus, sdSpace);

  menuRowsValid = true;
}

void term_markMenuPromptCursor(void) {
  menuPromptRow = cursorY;
  menuPromptCol = cursorX;
  menuPromptValid = true;
}

static void term_appendMoveAndClearLine(char *buffer, size_t bufferSize,
                                        size_t *offset, uint8_t row) {
  if ((buffer == NULL) || (offset == NULL) || (*offset >= bufferSize)) {
    return;
  }

  size_t remaining = bufferSize - *offset;
  int written = snprintf(buffer + *offset, remaining,
                         "\x1B"
                         "Y%c%c\x1B"
                         "K",
                         (char)(TERM_POS_Y + row), (char)(TERM_POS_X));
  if (written < 0) {
    return;
  }

  size_t writeSize = (size_t)written;
  if (writeSize >= remaining) {
    *offset = bufferSize - 1;
    return;
  }

  *offset += writeSize;
}

static void term_appendText(char *buffer, size_t bufferSize, size_t *offset,
                            const char *text) {
  if ((buffer == NULL) || (offset == NULL) || (text == NULL) ||
      (*offset >= bufferSize)) {
    return;
  }

  size_t remaining = bufferSize - *offset;
  int written = snprintf(buffer + *offset, remaining, "%s", text);
  if (written < 0) {
    return;
  }

  size_t writeSize = (size_t)written;
  if (writeSize >= remaining) {
    *offset = bufferSize - 1;
    return;
  }

  *offset += writeSize;
}

static bool term_buildLiveMenuLines(char *ssidLine, size_t ssidLineSize,
                                    char *selectLine, size_t selectLineSize,
                                    char *sdLine, size_t sdLineSize) {
  if ((ssidLine == NULL) || (selectLine == NULL) || (sdLine == NULL) ||
      (ssidLineSize == 0) || (selectLineSize == 0) || (sdLineSize == 0)) {
    return false;
  }

  char ssid[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char signalDb[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char selectState[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char sdStatus[TERM_NETWORK_INFO_VALUE_SIZE] = {0};
  char sdSpace[TERM_NETWORK_INFO_VALUE_SIZE] = {0};

  snprintf(ssid, sizeof(ssid), "N/A");
  snprintf(signalDb, sizeof(signalDb), "N/A");
  snprintf(selectState, sizeof(selectState), "%s",
           select_detectPush() ? "Pressed" : "Released");
  snprintf(sdStatus, sizeof(sdStatus), "Not mounted");
  snprintf(sdSpace, sizeof(sdSpace), "N/A");

  uint32_t sdTotalMb = 0;
  uint32_t sdFreeMb = 0;
  if (sdcard_getMountedInfo(&sdTotalMb, &sdFreeMb)) {
    snprintf(sdStatus, sizeof(sdStatus), "Mounted");
    snprintf(sdSpace, sizeof(sdSpace), "%lu/%lu MB free",
             (unsigned long)sdFreeMb, (unsigned long)sdTotalMb);
  } else if (sdcard_isMounted()) {
    snprintf(sdStatus, sizeof(sdStatus), "Error");
  }

#if defined(CYW43_WL_GPIO_LED_PIN)
  ip_addr_t currentIp = network_getCurrentIp();
  bool hasIp = !ip_addr_isany(&currentIp);
  if (hasIp) {
    wifi_network_info_t currentNetwork = network_getCurrentNetworkInfo();
    if (currentNetwork.ssid[0] != '\0') {
      snprintf(ssid, sizeof(ssid), "%s", currentNetwork.ssid);
    }
    if ((currentNetwork.rssi <= 0) && (currentNetwork.rssi >= -120)) {
      snprintf(signalDb, sizeof(signalDb), "%d dBm", currentNetwork.rssi);
    }
  }
#endif

  snprintf(ssidLine, ssidLineSize, "SSID      : %s (%s)", ssid, signalDb);
  snprintf(selectLine, selectLineSize, "SELECT  : %s", selectState);
  snprintf(sdLine, sdLineSize, "SD card   : %s (%s)", sdStatus, sdSpace);

  return true;
}

void term_refreshMenuLiveInfo(void) {
  static char prevSsidLine[TERM_MENU_LIVE_LINE_MAX] = {0};
  static char prevSelectLine[TERM_MENU_LIVE_LINE_MAX] = {0};
  static char prevSdLine[TERM_MENU_LIVE_LINE_MAX] = {0};

  char ssidLine[TERM_MENU_LIVE_LINE_MAX] = {0};
  char selectLine[TERM_MENU_LIVE_LINE_MAX] = {0};
  char sdLine[TERM_MENU_LIVE_LINE_MAX] = {0};

  if (!menuRowsValid ||
      !term_buildLiveMenuLines(ssidLine, sizeof(ssidLine), selectLine,
                               sizeof(selectLine), sdLine, sizeof(sdLine))) {
    return;
  }

  bool updateSsid = (strcmp(ssidLine, prevSsidLine) != 0);
  bool updateSelect = (strcmp(selectLine, prevSelectLine) != 0);
  bool updateSd = (strcmp(sdLine, prevSdLine) != 0);

  if (!updateSsid && !updateSelect && !updateSd) {
    return;
  }

  char updateBuffer[512] = {0};
  size_t offset = 0;

  if (updateSsid) {
    term_appendMoveAndClearLine(updateBuffer, sizeof(updateBuffer), &offset,
                                menuRowSsid);
    term_appendText(updateBuffer, sizeof(updateBuffer), &offset, ssidLine);
  }

  if (updateSelect) {
    term_appendMoveAndClearLine(updateBuffer, sizeof(updateBuffer), &offset,
                                menuRowSelect);
    term_appendText(updateBuffer, sizeof(updateBuffer), &offset, selectLine);
  }

  if (updateSd) {
    term_appendMoveAndClearLine(updateBuffer, sizeof(updateBuffer), &offset,
                                menuRowSd);
    term_appendText(updateBuffer, sizeof(updateBuffer), &offset, sdLine);
  }

  // Restore the cursor to the menu prompt input position.
  if (menuPromptValid && (offset < sizeof(updateBuffer))) {
    size_t remaining = sizeof(updateBuffer) - offset;
    int written = snprintf(updateBuffer + offset, remaining,
                           "\x1B"
                           "Y%c%c",
                           (char)(TERM_POS_Y + menuPromptRow),
                           (char)(TERM_POS_X + menuPromptCol));
    if (written > 0) {
      size_t writeSize = (size_t)written;
      if (writeSize >= remaining) {
        offset = sizeof(updateBuffer) - 1;
      } else {
        offset += writeSize;
      }
    }
  }

  snprintf(prevSsidLine, sizeof(prevSsidLine), "%s", ssidLine);
  snprintf(prevSelectLine, sizeof(prevSelectLine), "%s", selectLine);
  snprintf(prevSdLine, sizeof(prevSdLine), "%s", sdLine);

  term_printString(updateBuffer);
}

static bool term_parseKeyAndTail(const char *arg, char *key, size_t keySize,
                                 const char **tail) {
  if ((arg == NULL) || (key == NULL) || (keySize == 0)) {
    return false;
  }

  while (isspace((unsigned char)*arg)) {
    arg++;
  }
  if (*arg == '\0') {
    return false;
  }

  const char *end = arg;
  while ((*end != '\0') && !isspace((unsigned char)*end)) {
    end++;
  }

  size_t keyLen = (size_t)(end - arg);
  if ((keyLen == 0) || (keyLen >= keySize)) {
    return false;
  }

  memcpy(key, arg, keyLen);
  key[keyLen] = '\0';

  while (isspace((unsigned char)*end)) {
    end++;
  }

  if (tail != NULL) {
    *tail = end;
  }
  return true;
}

static bool term_parseBoolToken(const char *valueToken, bool *value) {
  if ((valueToken == NULL) || (value == NULL)) {
    return false;
  }

  char valueStr[TERM_BOOL_INPUT_BUFF] = {0};
  size_t valueLen = strcspn(valueToken, " \t\r\n");
  if ((valueLen == 0) || (valueLen >= sizeof(valueStr))) {
    return false;
  }
  memcpy(valueStr, valueToken, valueLen);
  valueStr[valueLen] = '\0';

  for (size_t i = 0; valueStr[i] != '\0'; i++) {
    valueStr[i] = (char)tolower((unsigned char)valueStr[i]);
  }

  if ((strcmp(valueStr, "true") == 0) || (strcmp(valueStr, "t") == 0) ||
      (strcmp(valueStr, "1") == 0)) {
    *value = true;
    return true;
  }
  if ((strcmp(valueStr, "false") == 0) || (strcmp(valueStr, "f") == 0) ||
      (strcmp(valueStr, "0") == 0)) {
    *value = false;
    return true;
  }

  return false;
}

// Command handlers
void term_cmdSettings(const char *arg) {
  term_printString(
      "\x1B"
      "E"
      "Available settings commands:\n");
  term_printString("  print   - Show settings\n");
  term_printString("  save    - Save settings\n");
  term_printString("  erase   - Erase settings\n");
  term_printString("  get     - Get setting (requires key)\n");
  term_printString("  put_int - Set integer (key and value)\n");
  term_printString("  put_bool- Set boolean (key and value)\n");
  term_printString("  put_str - Set string (key and value)\n");
  term_printString("\nEnter 'm' to return to the main menu.\n\n");
}

void term_cmdPrint(const char *arg) {
  char *buffer = (char *)malloc(TERM_PRINT_SETTINGS_BUFFER_SIZE);
  if (buffer == NULL) {
    term_printString("Error: Out of memory.\n");
    return;
  }
  settings_print(aconfig_getContext(), buffer);
  term_printString(buffer);
  free(buffer);
}

void term_cmdClear(const char *arg) { term_clearScreen(); }

void term_cmdExit(const char *arg) {
  term_printString("Exiting terminal...\n");
  // Send continue to desktop command
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
}

void term_cmdUnknown(const char *arg) {
  TPRINTF("Unknown command. Type 'help' for a list of commands.\n");
}

void term_cmdSave(const char *arg) {
  settings_save(aconfig_getContext(), true);
  term_printString("Settings saved.\n");
}

void term_cmdErase(const char *arg) {
  settings_erase(aconfig_getContext());
  term_printString("Settings erased.\n");
}

void term_cmdGet(const char *arg) {
  if (arg && strlen(arg) > 0) {
    SettingsConfigEntry *entry =
        settings_find_entry(aconfig_getContext(), &arg[0]);
    if (entry != NULL) {
      TPRINTF("Key: %s\n", entry->key);
      TPRINTF("Type: ");
      switch (entry->dataType) {
        case SETTINGS_TYPE_INT:
          TPRINTF("INT");
          break;
        case SETTINGS_TYPE_STRING:
          TPRINTF("STRING");
          break;
        case SETTINGS_TYPE_BOOL:
          TPRINTF("BOOL");
          break;
        default:
          TPRINTF("UNKNOWN");
          break;
      }
      TPRINTF("\n");
      TPRINTF("Value: %s\n", entry->value);
    } else {
      TPRINTF("Key not found.\n");
    }
  } else {
    TPRINTF("No key provided for 'get' command.\n");
  }
}

void term_cmdPutInt(const char *arg) {
  char key[SETTINGS_MAX_KEY_LENGTH] = {0};
  const char *valueStr = NULL;
  if (term_parseKeyAndTail(arg, key, sizeof(key), &valueStr) &&
      (valueStr != NULL) && (valueStr[0] != '\0')) {
    char *endPtr = NULL;
    long parsedValue = strtol(valueStr, &endPtr, DEC_BASE);
    while ((endPtr != NULL) && isspace((unsigned char)*endPtr)) {
      endPtr++;
    }
    if ((endPtr == valueStr) || ((endPtr != NULL) && (*endPtr != '\0')) ||
        (parsedValue < INT_MIN) || (parsedValue > INT_MAX)) {
      TPRINTF("Invalid arguments for 'put_int' command.\n");
      return;
    }

    int value = (int)parsedValue;
    int err = settings_put_integer(aconfig_getContext(), key, value);
    if (err == 0) {
      TPRINTF("Key: %s\n", key);
      TPRINTF("Value: %d\n", value);
    } else {
      TPRINTF("Error setting integer value for key: %s\n", key);
    }
  } else {
    TPRINTF("Invalid arguments for 'put_int' command.\n");
  }
}

void term_cmdPutBool(const char *arg) {
  char key[SETTINGS_MAX_KEY_LENGTH] = {0};
  const char *valueToken = NULL;
  bool value = false;

  if (term_parseKeyAndTail(arg, key, sizeof(key), &valueToken) &&
      (valueToken != NULL) && term_parseBoolToken(valueToken, &value)) {
    // Store the boolean value
    int err = settings_put_bool(aconfig_getContext(), key, value);
    if (err == 0) {
      TPRINTF("Key: %s\n", key);
      TPRINTF("Value: %s\n", value ? "true" : "false");
    } else {
      TPRINTF("Error setting boolean value for key: %s\n", key);
    }
  } else {
    TPRINTF(
        "Invalid arguments for 'put_bool' command. Usage: put_bool <key> "
        "<true/false>\n");
  }
}

void term_cmdPutString(const char *arg) {
  char key[SETTINGS_MAX_KEY_LENGTH] = {0};
  const char *value = NULL;

  if (term_parseKeyAndTail(arg, key, sizeof(key), &value)) {
    const char *safeValue = (value != NULL) ? value : "";
    int err = settings_put_string(aconfig_getContext(), key, safeValue);
    if (err == 0) {
      TPRINTF("Key: %s\n", key);
      TPRINTF("Value: %s\n", (safeValue[0] != '\0') ? safeValue : "<EMPTY>");
    } else {
      TPRINTF("Error setting string value for key: %s\n", key);
    }
  } else {
    TPRINTF("Invalid arguments for 'put_string' command.\n");
  }
}
