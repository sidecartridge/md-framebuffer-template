/**
 * File: select.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2025, February 2026
 * Copyright: 2024-2026 - GOODDATA LABS SL
 * Description: Header file for SELECT button detection functions
 */

#ifndef SELECT_H
#define SELECT_H

#include "constants.h"
#include "debug.h"
#include "pico/stdlib.h"

// SELECT is watched on core 0 only, by select_poll(). Core 1 stays off: a
// watcher on core 1 ran the long press's flash erase while core 0 executed
// from the same flash, and both cores faulted until a power cycle.

#define SELECT_DEBOUNCE_MS 30  // 30 ms stable level before accepting a change

#define SELECT_LONG_RESET 10000  // 10 seconds

// Define a callback typdef for the reset function
typedef void (*reset_callback_t)();

/**
 * @brief Initializes the SELECT detection.
 *
 * Configures the SELECT pin and a GPIO edge interrupt that records press and
 * release times. Call it on core 0, which also runs select_poll().
 */
void select_configure();

/**
 * @brief Detects button press.
 *
 * Returns the raw level of the SELECT pin: true while it is pressed.
 */
bool select_detectPush();

/**
 * @brief Runs the SELECT state machine; never blocks.
 *
 * Call it from the main loop and from every long wait. A press must be stable
 * for SELECT_DEBOUNCE_MS. The long callback runs once the button has been held
 * for SELECT_LONG_RESET; otherwise the short callback runs on release. A press
 * that started and ended while the caller was busy (seen by the edge
 * interrupt) is handled at the next call.
 */
void select_poll(void);

/**
 * @brief Registers the reset callback.
 *
 * Registers a callback to be invoked when a reset is requested by the
 * SELECT button event.
 *
 * Associates a function that will perform the necessary reset actions.
 */
void select_setResetCallback(reset_callback_t reset);

/**
 * @brief Registers the long reset callback.
 *
 * Associates a function that will be invoked when a long press reset is
 * requested by a prolonged SELECT button push.
 *
 * @param resetLong Callback function for a long press reset action.
 */
void select_setLongResetCallback(reset_callback_t resetLong);

#endif  // SELECT_H
