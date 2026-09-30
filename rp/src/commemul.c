/**
 * File: commemul.c
 * Author: Diego Parrilla Santamaría
 * Date: March 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: ROM3 communication emulator backed by a DMA ring buffer.
 */

#include "commemul.h"

#include "commemul.pio.h"
#include "constants.h"
#include "debug.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

// The ring holds what the ST reads in ROM3 while this side is not draining
// it: one blit ack per frame (50 a second) and the IKBD bytes, one sample
// each. The main loop drains it on every pass, and fb_publish() drains it
// while it waits for the ack. 2^12 bytes are 2,048 samples: about 40 s of
// acks, or 2.6 s of a mouse moved flat out (781 IKBD bytes a second). The
// ring must be aligned to its size for the DMA's address wrap, so a larger
// ring also wastes more RAM before it: at 32 KB the ring and its alignment
// padding took 46 KB, and the heap was left with 9 KB.
#define COMM_RING_BITS 12u
#define COMM_RING_SIZE_BYTES (1ul << COMM_RING_BITS)
#define COMM_RING_WORDS (COMM_RING_SIZE_BYTES / sizeof(uint16_t))
#define COMM_RING_MASK (COMM_RING_WORDS - 1u)
#define COMM_DMA_TRANSFER_COUNT (0xFFFFFFFFu)
// Re-arm the DMA channel once this many samples have been captured, long before
// the transfer count runs out and capture stops.
#define COMM_DMA_REARM_THRESHOLD (0x80000000u)

static uint16_t commRing[COMM_RING_WORDS]
    __attribute__((aligned(COMM_RING_SIZE_BYTES)));
static uint32_t commReadIdx = 0;
static int commDmaChannel = -1;
static int commSm = -1;
static bool commInitialized = false;
static PIO commPio = pio0;
// Samples captured since the channel was last armed, as of the last poll.
static uint32_t commLastWritten = 0;
// Ring index the channel was armed at.
static uint32_t commArmIdx = 0;
// Laps of the ring: times the reader fell a whole ring behind. Read over SWD
// by symbol, and through commemul_getOverruns().
uint32_t commOverruns = 0;

int commemul_init(void) {
  if (commInitialized) {
    return 0;
  }

  for (int i = 0; i < READ_ADDR_PIN_COUNT; i++) {
    pio_gpio_init(commPio, READ_ADDR_GPIO_BASE + i);
  }

  pio_gpio_init(commPio, READ_SIGNAL_GPIO_BASE);
  pio_gpio_init(commPio, WRITE_SIGNAL_GPIO_BASE);
  pio_gpio_init(commPio, ROM3_GPIO);

  gpio_set_dir(ROM3_GPIO, GPIO_IN);
  gpio_set_pulls(ROM3_GPIO, true, false);
  gpio_pull_up(ROM3_GPIO);

  int offset = pio_add_program(commPio, &commemul_read_program);
  if (offset < 0) {
    DPRINTF("commemul_init: pio_add_program failed (%d)\n", offset);
    return -1;
  }
  commSm = pio_claim_unused_sm(commPio, true);
  commemul_read_program_init(commPio, commSm, (uint)offset,
                             READ_ADDR_GPIO_BASE, READ_ADDR_PIN_COUNT,
                             READ_SIGNAL_GPIO_BASE, SAMPLE_DIV_FREQ);

  pio_sm_set_enabled(commPio, commSm, false);
  pio_sm_clear_fifos(commPio, commSm);
  pio_sm_restart(commPio, commSm);

  commDmaChannel = dma_claim_unused_channel(true);
  dma_channel_config c = dma_channel_get_default_config(commDmaChannel);
  channel_config_set_read_increment(&c, false);
  channel_config_set_write_increment(&c, true);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
  channel_config_set_ring(&c, true, COMM_RING_BITS);
  channel_config_set_dreq(&c, pio_get_dreq(commPio, commSm, false));

  dma_channel_configure(
      commDmaChannel, &c, commRing,
      (uint8_t *)(&commPio->rxf[commSm]) + 2,  // upper halfword of FIFO word
      COMM_DMA_TRANSFER_COUNT, true);

  pio_sm_set_enabled(commPio, commSm, true);
  commReadIdx = 0;
  commInitialized = true;

  DPRINTF("ROM3 comm ring initialized on pio0/sm%d dma%d (%u words / %u "
          "bytes)\n",
          commSm, commDmaChannel, (unsigned int)COMM_RING_WORDS,
          (unsigned int)COMM_RING_SIZE_BYTES);
  return 0;
}

bool __not_in_flash_func(commemul_latest)(uint16_t mask, uint16_t match,
                                          uint32_t max_back, uint16_t *sample) {
  if (!commInitialized) {
    return false;
  }
  uint32_t idx = ((dma_hw->ch[commDmaChannel].write_addr - (uint32_t)commRing) /
                  sizeof(uint16_t)) &
                 COMM_RING_MASK;
  for (uint32_t back = 0; back < max_back; back++) {
    idx = (idx - 1u) & COMM_RING_MASK;
    uint16_t value = commRing[idx];
    if ((value & mask) == match) {
      *sample = value;
      return true;
    }
  }
  return false;
}

void __not_in_flash_func(commemul_poll)(CommEmulSampleCallback callback) {
  if ((!commInitialized) || (callback == NULL)) {
    return;
  }

  uint32_t transfersWritten =
      COMM_DMA_TRANSFER_COUNT - dma_hw->ch[commDmaChannel].transfer_count;
  uint32_t writeIdx = (commArmIdx + transfersWritten) & COMM_RING_MASK;

  // When a whole ring or more arrived since the last poll (exactly a ring
  // reads as an empty one), the DMA lapped the reader and the unread samples
  // are a mix of old and new ones. Count it and drop them: the parser then
  // resynchronises on the next header.
  uint32_t unread = transfersWritten - commLastWritten;
  commLastWritten = transfersWritten;
  if (unread >= COMM_RING_WORDS) {
    commOverruns++;
    commReadIdx = writeIdx;
    return;
  }

  while (commReadIdx != writeIdx) {
    callback(commRing[commReadIdx]);
    commReadIdx = (commReadIdx + 1u) & COMM_RING_MASK;
  }

  if (transfersWritten >= COMM_DMA_REARM_THRESHOLD) {
    // Restart the channel where it is writing now, so the ring carries on.
    // The transfer count only means something while the channel runs, so the
    // position comes from its live write address. Samples written since the
    // drain above stay unread from commReadIdx, and samples arriving during
    // the restart wait in the PIO FIFO.
    dma_channel_abort(commDmaChannel);
    uint32_t idx =
        ((dma_hw->ch[commDmaChannel].write_addr - (uint32_t)commRing) /
         sizeof(uint16_t)) &
        COMM_RING_MASK;
    commArmIdx = idx;
    commLastWritten = 0;
    dma_channel_set_write_addr(commDmaChannel, &commRing[idx], false);
    dma_channel_set_trans_count(commDmaChannel, COMM_DMA_TRANSFER_COUNT, false);
    dma_channel_start(commDmaChannel);
    DPRINTF("commemul: DMA re-armed at ring index %lu\n", (unsigned long)idx);
  }
}

uint32_t commemul_getOverruns(void) { return commOverruns; }
