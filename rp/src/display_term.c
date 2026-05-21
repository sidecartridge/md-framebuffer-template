/**
 * File: display_term.c
 * Author: Diego Parrilla Santamaría
 * Date: January 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Terminal display functions.
 */

#include "display_term.h"

#include "display.h"

static uint8_t maxCol = DISPLAY_TILES_WIDTH;
static uint8_t maxRow = DISPLAY_TILES_HEIGHT;

// Static assert to ensure buffer size fits within uint32_t
_Static_assert(DISPLAY_BUFFER_SIZE <= UINT32_MAX,
               "Buffer size exceeds allowed limits");

void display_termChar(const uint8_t col, const uint8_t row, const char chr) {
  if ((col >= maxCol) || (row >= maxRow)) {
    return;
  }

  u8g2_DrawGlyph(
      display_getU8g2Ref(), col * DISPLAY_TERM_CHAR_WIDTH,
      (DISPLAY_TERM_FIRST_ROW_OFFSET + row) * DISPLAY_TERM_CHAR_HEIGHT, chr);
}

void display_termCursor(const uint8_t col, const uint8_t row) {
  if ((col >= maxCol) || (row >= maxRow)) {
    return;
  }

  // Cursor row offset is intentionally distinct from glyph row offset.
  u8g2_DrawBox(display_getU8g2Ref(), col * DISPLAY_TERM_CHAR_WIDTH,
               (DISPLAY_TERM_CURSOR_ROW_OFFSET + row) * DISPLAY_TERM_CHAR_HEIGHT,
               DISPLAY_TERM_CHAR_WIDTH,
               DISPLAY_TERM_CHAR_HEIGHT);
}

void display_termStart(const uint8_t numCol, const uint8_t numRow) {
  // Initialize the u8g2 library for a custom buffer
  display_setupU8g2();

  // // Clear the buffer first
  u8g2_ClearBuffer(display_getU8g2Ref());

  // Set the flag to NOT-RESET the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_NOP);

  display_refresh();

  // Set the max column and row
  maxCol = numCol;
  maxRow = numRow;

  DPRINTF("Created the term display\n");
}

void display_termRefresh() {
  // Refresh the display
  display_refresh();
}

void display_termClear() {
  // Clear the buffer
  u8g2_ClearBuffer(display_getU8g2Ref());
  u8g2_SetFont(display_getU8g2Ref(), u8g2_font_amstrad_cpc_extended_8f);
}
