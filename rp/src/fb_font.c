/**
 * File: fb_font.c
 * Description: Bitmap font renderer. Ported from md-sprites-demo's
 *              vga_font.c. Behavioural deltas from upstream:
 *               - Renders into `fb_screen.framebuffer` directly (no
 *                 hidden back buffer, single-FB design from Story 1.2 Q1).
 *               - The printf-family helpers (`font_printf`,
 *                 `font_print_int/uint/float`) were stripped so the
 *                 framebuffer text path doesn't pull newlib's
 *                 `vsnprintf` / `snprintf` into the binary. Apps that
 *                 need format strings either use `font_print()` with a
 *                 pre-formatted buffer (see `fb.c` for a hand-rolled
 *                 uint→decimal helper) or restore the upstream helpers
 *                 in their own module.
 */

#include <string.h>

#include "fb_draw.h"
#include "fb_font.h"

/* Renderer state declared extern in fb_font.h. */
struct FB_FONT const *font;
unsigned int font_x, font_y;
enum FONT_ALIGNMENT font_alignment;
unsigned char font_color;
unsigned char border[2];

static int __not_in_flash_func(render_text)(const char *text, int x, int y,
                                            unsigned int color) {
  if (!text || !*text) return x;

  const int screen_width = fb_screen.width;
  const int screen_height = fb_screen.height;
  const int glyph_w = font->w;
  const int glyph_h = font->h;
  const int first_char = font->first_char;
  const int last_char = first_char + font->num_chars; /* exclusive */
  const unsigned int color_mask = (1u << fb_screen.color_bits) - 1u;
  const unsigned int masked_color = color & color_mask;
  const int row_bytes = screen_width / 8; /* bytes per scanline block group */

  while (*text) {
    unsigned char ch = (unsigned char)*text++;
    if (ch < first_char || ch >= last_char) {
      x += glyph_w; /* advance even if unsupported */
      continue;
    }

    /* Horizontal offscreen reject. */
    int gx0 = x;
    int gx1 = x + glyph_w; /* exclusive */
    if (gx1 <= 0 || gx0 >= screen_width) {
      x += glyph_w;
      continue;
    }

    /* Vertical offscreen reject. */
    int gy0 = y;
    int gy1 = y + glyph_h; /* exclusive */
    if (gy1 <= 0 || gy0 >= screen_height) {
      x += glyph_w;
      continue;
    }

    int glyph_index = ch - first_char;
    int glyph_offset = glyph_index * glyph_h;
    const uint8_t *glyph_rows = &font->data[glyph_offset];

    /* Horizontal visible span (clip). */
    int vis_x0 = gx0 < 0 ? 0 : gx0;
    int vis_x1 = gx1 > screen_width ? screen_width : gx1;
    int vis_local_start = vis_x0 - gx0; /* first local pixel column */
    int vis_local_end = vis_x1 - gx0;   /* one past last local column */

    for (int row = 0; row < glyph_h; ++row) {
      int py = y + row;
      if (py < 0 || py >= screen_height) continue;
      uint8_t bits = glyph_rows[row];
      if (!bits) continue;

      uint8_t *line = (uint8_t *)&fb_screen.framebuffer[py * row_bytes];

      /* Iterate only visible columns; bit scanning skips unset pixels. */
      uint8_t masked_bits = (uint8_t)(bits & ((1u << glyph_w) - 1u));
      if (vis_local_start > 0)
        masked_bits &= (uint8_t)(0xFFu << vis_local_start);
      if (vis_local_end < glyph_w)
        masked_bits &= (uint8_t)((1u << vis_local_end) - 1u);
      while (masked_bits) {
        int local_bit =
            __builtin_ctz(masked_bits);   /* index of lowest set bit */
        masked_bits &= (masked_bits - 1); /* clear that bit */
        int px = gx0 + local_bit;
        if (px < vis_x0 || px >= vis_x1) continue;
        uint8_t *block = line + ((px >> 4) * 8);
        uint8_t pos = px & 0xF;
        uint64_t clear_mask = pixel_masks_flat[(0xF << 4) | pos];
        uint64_t set_mask = pixel_masks_flat[(masked_color << 4) | pos];
        uint64_t *planes64 = (uint64_t *)block;
        uint32_t old_lo = ((uint32_t *)planes64)[0];
        uint32_t old_hi = ((uint32_t *)planes64)[1];
        old_lo = (old_lo & ~((uint32_t)clear_mask)) | (uint32_t)set_mask;
        old_hi = (old_hi & ~((uint32_t)(clear_mask >> 32))) |
                 (uint32_t)(set_mask >> 32);
        ((uint32_t *)planes64)[0] = old_lo;
        ((uint32_t *)planes64)[1] = old_hi;
      }
    }
    x += glyph_w;
  }
  return x;
}

void __not_in_flash_func(font_print)(const char *text) {
  if (text == NULL) return;

  switch (font_alignment) {
    case FONT_ALIGN_LEFT:
      break;
    case FONT_ALIGN_CENTER:
      font_x -= strlen(text) * font->w / 2;
      break;
    case FONT_ALIGN_RIGHT:
      font_x -= strlen(text) * font->w;
      break;
  }

  if (border[0]) {
    for (int i = -1; i <= 1; i++) {
      for (int j = -1; j <= 1; j++) {
        if (i == 0 && j == 0) continue;
        render_text(text, font_x + i, font_y + j, border[1]);
      }
    }
  }
  int new_x = render_text(text, font_x, font_y, font_color);
  if (font_alignment != FONT_ALIGN_RIGHT) {
    font_x = new_x;
  }
}
