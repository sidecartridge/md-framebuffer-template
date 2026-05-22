/**
 * File: fb.h
 * Description: Framebuffer module — owns the single 32 KB cartridge
 *              framebuffer the m68k reads each VBL.
 *
 * Story 1.2 single-FB design (Q1): there is exactly one framebuffer in
 * the shared region (`$FA8300`, 32 KB, 320x200x4bpp). Double-buffering
 * happens on the ST side; the RP just writes into this one buffer.
 *
 * This header is introduced in Story 1.2.4 (alongside fb_font.* and
 * fb_draw.*) so the ported font/draw modules have a stable target
 * type. Story 1.2.5 adds fb.c which defines `fb_screen` and provides
 * the init / clear / address-accessor entry points.
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FB_MODE {
  unsigned short h_pixels;
  unsigned short v_pixels;
  unsigned char color_bits; /* bits per pixel */
};

/* Mirrors md-sprites-demo's VGA_SCREEN but with a single framebuffer
 * pointer instead of A/B/current/hidden (we don't keep a back buffer in
 * cartridge — Story 1.2 Q1). The ported text renderer writes into
 * `framebuffer` directly. */
struct FB_SCREEN {
  unsigned int *framebuffer;
  uint16_t width;
  uint16_t height;
  uint8_t color_bits;
  uint8_t _pad;
};

extern struct FB_SCREEN fb_screen;

/* Built-in 320x200 4 bpp Atari ST low-res mode. Defined in fb.c. */
extern const struct FB_MODE fb_mode_320x200;

/**
 * @brief Populate `fb_screen` from a mode descriptor, build the pixel
 *        mask LUT used by the text/draw primitives, and zero the
 *        framebuffer.
 *
 * @return 0 on success, -1 if `mode` is NULL.
 */
int fb_init(const struct FB_MODE *mode);

/** @brief Fill the cartridge framebuffer with 0xFF (solid black in
 *         the default TOS low-res palette). */
void fb_clear(void);

/** @brief Render the static parts of the template's boot UI (centered
 *         title + ESC hint). Called once by fb_init; the dynamic parts
 *         live in fb_render_frame. */
void fb_render_static(void);

/** @brief Render the dynamic parts of the template's boot UI: erase
 *         and redraw only the frame-counter row and the marquee row.
 *         Static title is left intact so we don't churn the parts of
 *         the FB the m68k is currently reading. Safe to call from the
 *         main loop at any cadence. */
void fb_render_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* FB_H */
