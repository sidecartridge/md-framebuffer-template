/**
 * File: display_term.h
 * Author: Diego Parrilla Santamaría
 * Date: January 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header file for the term mode diplay functions.
 */

#ifndef DISPLAY_TERM_H
#define DISPLAY_TERM_H

#include <stdint.h>

#ifdef DISPLAY_ATARIST
// Terminal size for Atari ST
// If the display of the chars is from bottom to top, then you need to add a
// ROW_OFFSET If it's top down, then set it to 0
#define DISPLAY_TERM_FIRST_ROW_OFFSET 1
// Keep cursor offset explicit and independent from glyph offset.
// Current behavior intentionally draws cursor one row above glyph baseline.
#define DISPLAY_TERM_CURSOR_ROW_OFFSET 0
#define DISPLAY_TERM_CHAR_WIDTH 8
#define DISPLAY_TERM_CHAR_HEIGHT 8
#endif

/**
 * @brief Draws a character glyph on the display buffer at the specified grid
 * position.
 *
 * This function calculates the pixel coordinates based on the provided column
 * and row indices, taking into account the character width, height, and a
 * predefined offset for the first row. It then uses the u8g2 graphics library
 * to render the specified character glyph on the display.
 *
 * @param col The column index where the character should be drawn. The actual
 * x-coordinate is computed as col multiplied by the character width.
 * @param row The row index where the character should be drawn. The actual
 * y-coordinate is computed as (DISPLAY_TERM_FIRST_ROW_OFFSET + row) multiplied
 * by the character height.
 * @param chr The character (glyph) to be displayed.
 */
void display_termChar(uint8_t col, uint8_t row, char chr);

/**
 * @brief Draws a solid block at the cursor position.
 *
 * This function renders a filled rectangular block on the display.
 * It calculates the position based on the provided column and row indices,
 * multiplied by the predefined character dimensions.
 * The y-coordinate intentionally uses DISPLAY_TERM_CURSOR_ROW_OFFSET, which
 * may differ from DISPLAY_TERM_FIRST_ROW_OFFSET used for glyphs.
 *
 * @param col The column index of the cursor position.
 * @param row The row index of the cursor position.
 */
void display_termCursor(uint8_t col, uint8_t row);

/**
 * @brief Initializes and starts the terminal display.
 *
 * This function performs the following operations:
 * - Initializes the u8g2 library with a custom buffer via display_setupU8g2().
 * - Clears the display buffer by calling u8g2_ClearBuffer().
 * - Sends a NOP command to the display to prevent a computer reset.
 * - Refreshes the display using display_refresh().
 * - Sets the maximum number of columns and rows for the terminal display.
 *
 * @param numCol The maximum number of columns for the terminal.
 * @param numRow The maximum number of rows for the terminal.
 */
void display_termStart(uint8_t numCol, uint8_t numRow);

/**
 * @brief Refresh the terminal display.
 *
 * This function refreshes the terminal display by calling the underlying
 * display_refresh() function, which updates the visual content on the screen.
 */
void display_termRefresh();

/**
 * @brief Clears the terminal display buffer and sets the font.
 *
 * This function clears the current display buffer and sets the font to
 * 'u8g2_font_amstrad_cpc_extended_8f' for the terminal display.
 */
void display_termClear();
#endif  // DISPLAY_TERM_H
