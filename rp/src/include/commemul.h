/**
 * File: commemul.h
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: ROM3 communication emulator backed by a DMA ring buffer.
 */

#ifndef COMMEMUL_H
#define COMMEMUL_H

#include <inttypes.h>
#include <stdbool.h>

#include "pico/stdlib.h"

typedef void (*CommEmulSampleCallback)(uint16_t sample);

// Returns 0 on success, < 0 on failure (PIO program load failed). The
// PIO state-machine and DMA-channel claims call the SDK's "panic on
// exhaustion" variants, so those paths abort the whole boot rather
// than returning here.
int commemul_init(void);
void __not_in_flash_func(commemul_poll)(CommEmulSampleCallback callback);

// Times the reader fell a whole ring behind the capture and the unread samples
// were dropped (also readable over SWD as commOverruns).
uint32_t commemul_getOverruns(void);

// The most recent sample in the ring with (sample & mask) == match, looking
// back at most max_back samples from where the capture is writing now.
// Read-only: it neither consumes samples nor disturbs commemul_poll(), so
// an interrupt handler may call it. Returns false when none is found.
bool __not_in_flash_func(commemul_latest)(uint16_t mask, uint16_t match,
                                          uint32_t max_back, uint16_t *sample);

#endif  // COMMEMUL_H
