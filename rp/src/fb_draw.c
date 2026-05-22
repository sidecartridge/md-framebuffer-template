/**
 * File: fb_draw.c
 * Description: Defines the pixel-mask lookup the text renderer pokes
 *              individual pixels through. Ported from md-sprites-demo's
 *              vga_draw.c; sprite / tile / color-index machinery left
 *              behind per Story 1.2 Q2/Q5.
 */

#include "fb_draw.h"

uint64_t pixel_masks_flat[FB_PIXEL_MASK_TABLE_SIZE] __attribute__((
    aligned(8),
    section(".scratch_x.pixel_masks")));  // flattened [palette<<4 | pixel_x]

void __not_in_flash_func(init_pixel_masks)(void) {
  for (int index = 0; index < 16; ++index) {
    for (int x = 0; x < 16; ++x) {
      uint64_t mask = 0;
      for (int plane = 0; plane < FB_NUM_BITPLANES; ++plane) {
        if (index & (1 << plane)) {
          mask |= (uint64_t)1 << (plane * 16 + (15 - x));
        }
      }
      int flat = (index << 4) | x;
      pixel_masks_flat[flat] = mask;
    }
  }
}
