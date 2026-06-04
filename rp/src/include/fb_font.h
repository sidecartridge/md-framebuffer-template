/**
 * File: fb_font.h
 * Description: Bitmap font renderer ported from md-sprites-demo's
 *              `rp/src/include/vga/font.h`.
 *
 * Public state (extern globals declared below) and inline setters are
 * kept verbatim from upstream so font assets ported alongside this
 * module (e.g. font6x8.h) drop in without modification. The renderer
 * writes directly into `fb_screen.framebuffer` (single-FB design)
 * — there is no back buffer here.
 */

#ifndef FB_FONT_H
#define FB_FONT_H

#include <stdint.h>

#include "fb.h"
#include "pico/stdlib.h"

#ifdef __cplusplus
extern "C" {
#endif

struct FB_FONT {
  int w;                     /* glyph width in pixels */
  int h;                     /* glyph height in pixels */
  int first_char;            /* first ASCII codepoint represented */
  int num_chars;             /* count of sequential characters */
  const unsigned char *data; /* bitmap rows: (h rows) * num_chars */
};

enum FONT_ALIGNMENT { FONT_ALIGN_LEFT, FONT_ALIGN_CENTER, FONT_ALIGN_RIGHT };

#define FONT_BORDER_COLOR_MASK 0x1F

static inline __attribute__((always_inline)) unsigned int
font_active_color_mask(void) {
  return (1u << fb_screen.color_bits) - 1u;
}

/* Renderer state — defined in fb_font.c. */
extern struct FB_FONT const *font;
extern unsigned int font_x, font_y;
extern enum FONT_ALIGNMENT font_alignment;
extern unsigned char font_color;
extern unsigned char border[2];

static inline void __not_in_flash_func(font_set_font)(
    const struct FB_FONT *newFont) {
  font = newFont;
}
static inline void __not_in_flash_func(font_set_color)(unsigned int fgColor) {
  font_color = (unsigned char)(fgColor & font_active_color_mask());
}
static inline void __not_in_flash_func(font_set_border)(
    int enableBorder, unsigned int borderColor) {
  border[0] = (unsigned char)enableBorder;
  border[1] = (unsigned char)(borderColor & FONT_BORDER_COLOR_MASK);
}
static inline void __not_in_flash_func(font_move)(unsigned int pos_x,
                                                  unsigned int pos_y) {
  font_x = pos_x;
  font_y = pos_y;
}
static inline void __not_in_flash_func(font_align)(
    enum FONT_ALIGNMENT alignment) {
  font_alignment = alignment;
}

/* Print a pre-formatted string. printf-family helpers (font_printf /
 * font_print_int / font_print_uint / font_print_float) were stripped
 * to keep newlib's vsnprintf / snprintf out of the binary. See
 * fb_font.c for the rationale and fb.c's `fb_fmt_uint` for the
 * recommended pattern. */
void __not_in_flash_func(font_print)(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* FB_FONT_H */
