/**
 * File: demo_menu.c
 * Description: Boot menu + demo dispatcher.
 *
 * Owns the boot menu UI (rendered into the same chunked framebuffer
 * the demos use) and the MENU <-> ACTIVE state machine.
 *
 * ESC routing:
 *   - MENU + ESC = exit to GEM (CMD_BOOT_GEM via cart sentinel).
 *   - ACTIVE + ESC = teardown current demo, return to MENU.
 *
 * The dispatcher opts out of ikbd.c's built-in ESC auto-exit during
 * init so it can own the key end-to-end.
 */

#include "demo.h"

#include <stdint.h>

#include "audio.h"
#include "audio_sample.h"
#include "cart_shared.h"
#include "debug.h"
#include "fb.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "fb_blit.h"
#include "hardware/interp.h"
#include "ikbd.h"
#include "memfunc.h"
#include "palette.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "sidecart_logo.h"
#include "sidecart_text.h"
#include "st_session.h"

/* Per-file -O3: the menu now runs a per-pixel rotozoom backdrop.
 * Pure compute -- no bus/PIO timing code here -- so opt it for speed. */
#pragma GCC optimize("O3")

/* font8x8 is defined in fb.c (the canonical single point that
 * includes the heavy font8x8.h glyph data). Demos just use it. */
extern const struct FB_FONT font8x8;

/* sin/cos table: 256 steps over 2*pi, scaled to +-256 (8.8). */
static const int16_t sin256[256] = {
    0,    6,    13,   19,   25,   31,   38,   44,   50,   56,   62,   68,
    74,   80,   86,   92,   98,   104,  109,  115,  121,  126,  132,  137,
    142,  147,  152,  157,  162,  167,  172,  177,  181,  185,  190,  194,
    198,  202,  206,  209,  213,  216,  220,  223,  226,  229,  231,  234,
    237,  239,  241,  243,  245,  247,  248,  250,  251,  252,  253,  254,
    255,  255,  256,  256,  256,  256,  256,  255,  255,  254,  253,  252,
    251,  250,  248,  247,  245,  243,  241,  239,  237,  234,  231,  229,
    226,  223,  220,  216,  213,  209,  206,  202,  198,  194,  190,  185,
    181,  177,  172,  167,  162,  157,  152,  147,  142,  137,  132,  126,
    121,  115,  109,  104,  98,   92,   86,   80,   74,   68,   62,   56,
    50,   44,   38,   31,   25,   19,   13,   6,    0,    -6,   -13,  -19,
    -25,  -31,  -38,  -44,  -50,  -56,  -62,  -68,  -74,  -80,  -86,  -92,
    -98,  -104, -109, -115, -121, -126, -132, -137, -142, -147, -152, -157,
    -162, -167, -172, -177, -181, -185, -190, -194, -198, -202, -206, -209,
    -213, -216, -220, -223, -226, -229, -231, -234, -237, -239, -241, -243,
    -245, -247, -248, -250, -251, -252, -253, -254, -255, -255, -256, -256,
    -256, -256, -256, -255, -255, -254, -253, -252, -251, -250, -248, -247,
    -245, -243, -241, -239, -237, -234, -231, -229, -226, -223, -220, -216,
    -213, -209, -206, -202, -198, -194, -190, -185, -181, -177, -172, -167,
    -162, -157, -152, -147, -142, -137, -132, -126, -121, -115, -109, -104,
    -98,  -92,  -86,  -80,  -74,  -68,  -62,  -56,  -50,  -44,  -38,  -31,
    -25,  -19,  -13,  -6,
};

/* Menu animation state + its own 16-colour palette (re-published every
 * frame so the logo colours can "beat"). idx 0 = white (text), 1..4 =
 * the motorcycle-logo shade ramp (cycled per frame), 15 = dark backdrop
 * (also the rotozoom's transparent index). */
static uint32_t s_menu_frame;
static uint16_t s_menu_palette[PALETTE_ENTRIES];

/* Shared DRAW + C2P timing readout toggle (declared in demo.h, gated by
 * the 'D' key in any state -- menu and every demo). Starts ON. */
bool g_show_timing = true;
static uint32_t s_prev_draw_us;        /* previous frame's menu DRAW us */
static uint32_t s_prev_cv_us;          /* previous frame's C2P us */

/* uint32 -> decimal (fb_font has no printf). */
static const char *u32str(uint32_t n, char *buf, int buf_sz) {
  char *p = buf + buf_sz;
  *--p = '\0';
  if (n == 0) {
    *--p = '0';
  } else {
    while (n) {
      *--p = (char)('0' + (n % 10));
      n /= 10;
    }
  }
  return p;
}

typedef enum {
  DEMO_STATE_MENU,
  DEMO_STATE_ACTIVE,
  DEMO_STATE_EXITING,
} demo_state_t;

static demo_state_t s_state;
static const demo_module_t *s_active;
static int s_menu_sel; /* highlighted menu item (0..MENU_ITEM_COUNT-1) */

#define MENU_ITEM_COUNT 7
static const demo_module_t *const s_menu[MENU_ITEM_COUNT] = {
    &demo_parallax,
    &demo_3d,
    &demo_sprites,
    &demo_cojorotozoom,
    &demo_input,
    &demo_arena,
    &demo_zap,
};

/* IKBD scancodes for the menu hotkeys. ESC = $01 is the back/exit
 * key; 1..7 = $02..$08 on the unshifted top row. */
#define IKBD_SC_ESC 0x01u
#define IKBD_SC_1   0x02u
#define IKBD_SC_7   0x08u
#define IKBD_SC_D   0x20u  /* hidden: toggle DRAW/C2P readout (menu + demos) */
#define IKBD_SC_B   0x30u  /* B = return to Booster (menu only) */
#define IKBD_SC_RET 0x1Cu  /* Return = launch the highlighted item */
#define IKBD_SC_UP  0x48u  /* move selection up */
#define IKBD_SC_DOWN 0x50u /* move selection down */

/* Beating logo palette: the silhouette ramp (idx 1..4) cycles through the
 * spectrum via three phase-shifted sines (classic plasma colour); idx 0
 * stays white for text, idx 15 is a dark backdrop. Re-published each
 * frame so the colours "beat". */
/* Peak logo brightness on the 0..7 ST scale -- kept low for a dim look. */
#define MENU_MAXB 5

/* One logo ramp: a cycling hue (three phase-shifted sines) gated by a
 * brightness envelope that dips all the way to 0, so the logo beats
 * bright -> dim -> BLACK and back. `hue_ph` shifts the hue, `env_ph`
 * shifts the envelope. Writes the 4-shade ramp into pal[base..base+3]. */
static void menu_logo_ramp(uint16_t *pal, int base, uint32_t f, int hue_ph,
                           int env_ph) {
  int env = sin256[(f + env_ph) & 255] + 256;            /* 0..512 */
  int hr = sin256[((f >> 2) + hue_ph) & 255] + 256;       /* 0..512 */
  int hg = sin256[((f >> 2) + hue_ph + 85) & 255] + 256;
  int hb = sin256[((f >> 2) + hue_ph + 170) & 255] + 256;
  int r = (hr * env * MENU_MAXB) >> 18;                   /* 0..MENU_MAXB */
  int g = (hg * env * MENU_MAXB) >> 18;
  int b = (hb * env * MENU_MAXB) >> 18;
  pal[base + 0] = PALETTE_RGB(r >> 2, g >> 2, b >> 2);    /* edge */
  pal[base + 1] = PALETTE_RGB(r >> 1, g >> 1, b >> 1);
  pal[base + 2] = PALETTE_RGB((r * 3) >> 2, (g * 3) >> 2, (b * 3) >> 2);
  pal[base + 3] = PALETTE_RGB(r, g, b);                   /* core */
}

static void menu_beat_palette(uint32_t f) {
  for (int i = 0; i < PALETTE_ENTRIES; i++) {
    s_menu_palette[i] = PALETTE_RGB(0, 0, 0);
  }
  s_menu_palette[0] = PALETTE_RGB(7, 7, 7);    /* text white */
  /* Motorcycle ramp (idx 1..4) and wordmark ramp (idx 5..8) on opposite
   * envelope phases (env_ph 0 vs 128), so when one logo fades to black
   * the other is at full beat -- the screen is never fully dark. */
  menu_logo_ramp(s_menu_palette, 1, f, 0, 0);
  menu_logo_ramp(s_menu_palette, 5, f, 128, 128);
  s_menu_palette[14] = PALETTE_RGB(7, 4, 0);   /* selection highlight (amber) */
  s_menu_palette[15] = PALETTE_RGB(0, 0, 1);   /* dark backdrop */
  palette_set(s_menu_palette);
}

static void __not_in_flash_func(render_menu)(void) {
  uint32_t t0 = time_us_32();
  uint32_t f = s_menu_frame++;
  menu_beat_palette(f);

  /* Rotozoom the SidecarTridge logo full-screen via INTERP0 as the texel
   * address generator (same technique as the cojorotozoom demo; 128x128
   * texture). The texture's transparent index (15) is just the dark
   * backdrop colour, so every screen pixel is written -- no clear. */
  int ang = (int)(f) & 255; /* mid spin (between f>>1 and f*2) */
  int cos_a = sin256[(ang + 64) & 255];
  int sin_a = sin256[ang];
  int z = 420 + ((220 * sin256[(f >> 2) & 255]) >> 8); /* zoom pulse, 8.8 */
  int dux = (cos_a * z) >> 8;
  int duy = (-sin_a * z) >> 8;
  int dvx = (sin_a * z) >> 8;
  int dvy = (cos_a * z) >> 8;
  int cu = ((SIDECART_LOGO_W / 2) << 8) + (sin256[(f * 2) & 255] << 3);
  int cv = ((SIDECART_LOGO_H / 2) << 8) + (sin256[((f * 2) + 64) & 255] << 3);
  int u_row = cu - 160 * dux - 100 * duy;
  int v_row = cv - 160 * dvx - 100 * dvy;

  interp_config c0 = interp_default_config();
  interp_config_set_shift(&c0, 8);
  interp_config_set_mask(&c0, 0, 6); /* (u>>8) & 127 */
  interp_set_config(interp0, 0, &c0);
  interp_config c1 = interp_default_config();
  interp_config_set_shift(&c1, 1);
  interp_config_set_mask(&c1, 7, 13); /* ((v>>8) & 127) << 7 */
  interp_set_config(interp0, 1, &c1);
  interp0->base[2] = (uintptr_t)sidecart_logo_data;

  for (int sy = 0; sy < FB_CHUNKED_H; sy++) {
    interp0->accum[0] = (uint32_t)u_row;
    interp0->accum[1] = (uint32_t)v_row;
    uint8_t *row = fb_chunked_buffer + (size_t)sy * FB_CHUNKED_W;
    for (int sx = 0; sx < FB_CHUNKED_W; sx++) {
      row[sx] = *(const uint8_t *)interp_peek_full_result(interp0);
      interp_add_accumulator(interp0, 0, (uint32_t)dux);
      interp_add_accumulator(interp0, 1, (uint32_t)dvx);
    }
    u_row += duy;
    v_row += dvy;
  }

  /* Layer 2: the "SidecarTridge" wordmark rotozoom (256x64 texture, the
   * cojo interp config), counter-rotating at a different zoom, composited
   * TRANSPARENT over the motorcycle (only its silhouette texels, idx
   * 5..8, are written; idx 15 = transparent lets layer 1 show through). */
  int ang2 = (256 - (int)((f * 2) & 255)) & 255; /* mid opposite spin */
  int cos2 = sin256[(ang2 + 64) & 255];
  int sin2 = sin256[ang2];
  int z2 = 360 + ((180 * sin256[((f >> 2) + 64) & 255]) >> 8);
  int dux2 = (cos2 * z2) >> 8;
  int duy2 = (-sin2 * z2) >> 8;
  int dvx2 = (sin2 * z2) >> 8;
  int dvy2 = (cos2 * z2) >> 8;
  int cu2 = ((SIDECART_TEXT_W / 2) << 8) + (sin256[(f * 3) & 255] << 3);
  int cv2 = ((SIDECART_TEXT_H / 2) << 8) + (sin256[((f * 3) + 64) & 255] << 3);
  int u2 = cu2 - 160 * dux2 - 100 * duy2;
  int v2 = cv2 - 160 * dvx2 - 100 * dvy2;

  interp_config_set_shift(&c0, 8);
  interp_config_set_mask(&c0, 0, 7); /* (u>>8) & 255 */
  interp_set_config(interp0, 0, &c0);
  interp_config_set_shift(&c1, 0);
  interp_config_set_mask(&c1, 8, 13); /* ((v>>8) & 63) << 8 */
  interp_set_config(interp0, 1, &c1);
  interp0->base[2] = (uintptr_t)sidecart_text_data;

  for (int sy = 0; sy < FB_CHUNKED_H; sy++) {
    interp0->accum[0] = (uint32_t)u2;
    interp0->accum[1] = (uint32_t)v2;
    uint8_t *row = fb_chunked_buffer + (size_t)sy * FB_CHUNKED_W;
    for (int sx = 0; sx < FB_CHUNKED_W; sx++) {
      uint8_t texel = *(const uint8_t *)interp_peek_full_result(interp0);
      if (texel != SIDECART_TEXT_TRANSPARENT) {
        row[sx] = texel;
      }
      interp_add_accumulator(interp0, 0, (uint32_t)dux2);
      interp_add_accumulator(interp0, 1, (uint32_t)dvx2);
    }
    u2 += duy2;
    v2 += dvy2;
  }

  /* Dark panel behind the menu list for legibility, centred on the
   * screen, then the text. */
  fb_fill_rect(48, 38, 224, 124, 15);

  font_set_font(&font8x8);
  font_set_color(0);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_CENTER);
  font_move(FB_CHUNKED_W / 2, 12);
  font_print("S I D E C A R T R I D G E");

  static const char *const items[MENU_ITEM_COUNT] = {
      "1.  Uridium scroll", "2.  3D Solid",  "3.  Multi-sprite swarm",
      "4.  Cojorotozoom",   "5.  Input test", "6.  Arena (joystick)",
      "7.  Zap (mouse)"};
  font_align(FONT_ALIGN_LEFT);
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    int iy = 46 + i * 13;
    if (i == s_menu_sel) {
      fb_fill_rect(56, iy - 2, 208, 12, 14); /* amber highlight bar */
      font_set_color(15);                    /* dark text on the bar */
    } else {
      font_set_color(0);                     /* white text */
    }
    font_move(72, iy);
    font_print(items[i]);
  }
  font_set_color(0);
  font_move(60, 140);
  font_print("UP/DN  RET=start  ESC=exit");
  font_align(FONT_ALIGN_CENTER);
  font_move(FB_CHUNKED_W / 2, 151);
  font_print("B=return to Booster");

  /* Hidden DRAW/C2P readout (toggled with 'D'), previous frame's numbers
   * on a dark strip at the bottom. */
  if (g_show_timing) {
    char num[11];
    fb_fill_rect(0, FB_CHUNKED_H - 12, 210, 12, 15);
    font_align(FONT_ALIGN_LEFT);
    font_move(6, FB_CHUNKED_H - 10);
    font_print("DRAW ");
    font_print(u32str(s_prev_draw_us, num, sizeof(num)));
    font_print(" C2P ");
    font_print(u32str(s_prev_cv_us, num, sizeof(num)));
    font_print(" US");
  }

  uint32_t draw_us = time_us_32() - t0;
  fb_publish();
  s_prev_draw_us = draw_us;
  s_prev_cv_us = fb_last_convert_us();
}

static void exit_to_gem(void) {
  volatile uint32_t *sentinel =
      (volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                            CART_CMD_SENTINEL_OFFSET);
  *sentinel = cart_asM68kLong(CART_CMD_BOOT_GEM);
  s_state = DEMO_STATE_EXITING;
}

/* 'B': a last frame that says what happens -- the ST shows it while it
 * waits for its cold reset -- then leave for Booster. */
static void return_to_booster(void) {
  fb_fill_rect(0, 0, FB_CHUNKED_W, FB_CHUNKED_H, 15);
  font_set_font(&font8x8);
  font_set_color(0);
  font_align(FONT_ALIGN_CENTER);
  font_move(FB_CHUNKED_W / 2, FB_CHUNKED_H / 2 - 4);
  font_print("Returning to Booster...");
  fb_publish();
  st_session_return_to_booster();
}

void demo_menu_music(void) {
  /* A .YMS file from the app folder first; on any failure (no SD, file
   * missing, bad header, rate mismatch) the baked-in Ghostbusters G1
   * jingle, so the menu always has sound. */
  if (audio_play_yms_file("DEMO.YMS") < 0) {
    audio_play_loop(audio_sample_data, (uint32_t)sizeof(audio_sample_data));
  }
}

void demo_dispatcher_init(void) {
  s_state = DEMO_STATE_MENU;
  s_active = NULL;
  demo_menu_music();
  /* Take ownership of the ESC key from ikbd.c -- the dispatcher
   * routes it to "back to menu" while a demo is active and uses it
   * to exit to GEM only from the menu. */
  ikbd_set_esc_auto_exit(false);
  DPRINTF("demo_dispatcher_init: MENU state, ESC owned by dispatcher\n");
}

/* Back to the menu, whatever was left -- a demo running, or the dispatcher
 * waiting for the exit to GEM. */
static void back_to_menu(void) {
  if (s_state == DEMO_STATE_ACTIVE && s_active && s_active->teardown) {
    s_active->teardown();
  }
  s_active = NULL;
  s_state = DEMO_STATE_MENU;
}

/* A new ST session (st_session.h): back to the menu. */
void demo_dispatcher_restart(void) {
  back_to_menu();
  DPRINTF("dispatcher: new ST session -> menu\n");
}

/* Launch the demo at menu index `idx` (shared by the number keys and
 * Return on the highlighted item). */
static void launch_demo(unsigned idx) {
  if (idx >= MENU_ITEM_COUNT) return;
  s_active = s_menu[idx];
  s_state = DEMO_STATE_ACTIVE;
  DPRINTF("dispatcher: starting demo '%s'\n",
          s_active->name ? s_active->name : "(unnamed)");
  if (s_active->init) {
    s_active->init();
  }
}

void demo_dispatcher_handle_key(const ikbd_key_event_t *k) {
  if (!k->is_press) {
    /* Forward releases to the active demo (it may track held keys);
     * the menu only cares about presses. */
    if (s_state == DEMO_STATE_ACTIVE && s_active && s_active->handle_key) {
      s_active->handle_key(k);
    }
    return;
  }

  /* 'D' toggles the DRAW/C2P timing readout globally -- works in the
   * menu and inside any demo (no demo binds 'D'). */
  if (k->scancode == IKBD_SC_D) {
    g_show_timing = !g_show_timing;
    DPRINTF("dispatcher: timing readout %s\n", g_show_timing ? "ON" : "OFF");
    return;
  }

  if (s_state == DEMO_STATE_MENU) {
    switch (k->scancode) {
      case IKBD_SC_ESC:
        DPRINTF("dispatcher: ESC from menu -> exit to GEM\n");
        exit_to_gem();
        break;
      case IKBD_SC_B:
        DPRINTF("dispatcher: B from menu -> return to Booster\n");
        return_to_booster();
        break;
      case IKBD_SC_UP:
        s_menu_sel = (s_menu_sel + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        break;
      case IKBD_SC_DOWN:
        s_menu_sel = (s_menu_sel + 1) % MENU_ITEM_COUNT;
        break;
      case IKBD_SC_RET:
        launch_demo((unsigned)s_menu_sel);
        break;
      default:
        if (k->scancode >= IKBD_SC_1 && k->scancode <= IKBD_SC_7) {
          launch_demo((unsigned)(k->scancode - IKBD_SC_1));
        }
        break;
    }
    return;
  }

  if (s_state == DEMO_STATE_ACTIVE) {
    if (k->scancode == IKBD_SC_ESC) {
      DPRINTF("dispatcher: ESC from demo '%s' -> back to menu\n",
              s_active && s_active->name ? s_active->name : "(unnamed)");
      if (s_active && s_active->teardown) {
        s_active->teardown();
      }
      s_active = NULL;
      s_state = DEMO_STATE_MENU;
      return;
    }
    if (s_active && s_active->handle_key) {
      s_active->handle_key(k);
    }
  }
}

#if defined(_DEBUG) && (_DEBUG != 0)
/* A test tone through the PCM path (DEVHOOKS_APP_TONE): a sine, whose
 * clicks and jitter are easy to hear, on either output. */
static const int8_t k_tone_sine[256] = {
    0, 2, 5, 7, 10, 12, 15, 17, 20, 22, 24, 27, 29, 31, 34, 36,
    38, 41, 43, 45, 47, 49, 51, 53, 56, 58, 60, 62, 63, 65, 67, 69,
    71, 72, 74, 76, 77, 79, 80, 82, 83, 84, 86, 87, 88, 89, 90, 91,
    92, 93, 94, 95, 96, 96, 97, 98, 98, 99, 99, 99, 100, 100, 100, 100,
    100, 100, 100, 100, 100, 99, 99, 99, 98, 98, 97, 96, 96, 95, 94, 93,
    92, 91, 90, 89, 88, 87, 86, 84, 83, 82, 80, 79, 77, 76, 74, 72,
    71, 69, 67, 65, 63, 62, 60, 58, 56, 53, 51, 49, 47, 45, 43, 41,
    38, 36, 34, 31, 29, 27, 24, 22, 20, 17, 15, 12, 10, 7, 5, 2,
    0, -2, -5, -7, -10, -12, -15, -17, -20, -22, -24, -27, -29, -31, -34, -36,
    -38, -41, -43, -45, -47, -49, -51, -53, -56, -58, -60, -62, -63, -65, -67, -69,
    -71, -72, -74, -76, -77, -79, -80, -82, -83, -84, -86, -87, -88, -89, -90, -91,
    -92, -93, -94, -95, -96, -96, -97, -98, -98, -99, -99, -99, -100, -100, -100, -100,
    -100, -100, -100, -100, -100, -99, -99, -99, -98, -98, -97, -96, -96, -95, -94, -93,
    -92, -91, -90, -89, -88, -87, -86, -84, -83, -82, -80, -79, -77, -76, -74, -72,
    -71, -69, -67, -65, -63, -62, -60, -58, -56, -53, -51, -49, -47, -45, -43, -41,
    -38, -36, -34, -31, -29, -27, -24, -22, -20, -17, -15, -12, -10, -7, -5, -2,
};
static uint32_t s_tone_phase;
static uint32_t s_tone_step;

static void tone_cb(int8_t *buf, uint32_t samples) {
  for (uint32_t i = 0; i < samples; i++) {
    buf[i] = k_tone_sine[s_tone_phase >> 24];
    s_tone_phase += s_tone_step;
  }
}

/* Extra time every frame takes, set from the host (DEVHOOKS_APP_SLOW_FRAME):
 * an app late with its frames, on demand. The sound and the publish
 * handshake must survive it. */
static uint32_t s_slow_frame_us;

uint32_t demo_dispatcher_devhook(uint16_t commandId, const uint16_t *payload,
                                 uint16_t payloadSize) {
  uint16_t arg = payloadSize >= 2u ? payload[0] : 0u;
  switch (commandId) {
    case DEVHOOKS_APP_DEMO:
      if (arg < 1u || arg > MENU_ITEM_COUNT) return 0;
      back_to_menu();
      launch_demo(arg - 1u);
      return 1;
    case DEVHOOKS_APP_MENU:
      DPRINTF("dispatcher: host -> menu\n");
      back_to_menu();
      return 1;
    case DEVHOOKS_APP_OVERLAY:
      g_show_timing = arg != 0u;
      return 1;
    case DEVHOOKS_APP_SLOW_FRAME:
      s_slow_frame_us = (uint32_t)arg * 1000u;
      DPRINTF("dispatcher: every frame stalls %u ms\n", (unsigned)arg);
      return 1;
    case DEVHOOKS_APP_INPUT_MODE:
      if (arg >= IKBD_INPUT_MODES) return 0;
      DPRINTF("dispatcher: host -> input mode %u\n", (unsigned)arg);
      ikbd_set_input_mode((ikbd_input_mode_t)arg);
      return 1;
    case DEVHOOKS_APP_AUDIO_OUT:
      audio_prefer_ym(arg != 0u);
      return 1;
    case DEVHOOKS_APP_TONE:
      if (arg == 0u) {
        DPRINTF("dispatcher: host -> tone off, DEMO.YMS\n");
        return audio_play_yms_file("DEMO.YMS") == 0 ? 1u : 0u;
      }
      DPRINTF("dispatcher: host -> tone %u Hz\n", (unsigned)arg);
      s_tone_step = (uint32_t)(((uint64_t)arg << 32) / AUDIO_DMA_RATE_HZ);
      audio_set_pcm_callback(tone_cb, AUDIO_DMA_RATE_HZ);
      return 1;
    case DEVHOOKS_APP_COPY_MODE: {
      uint16_t piece = payloadSize >= 4u ? payload[1] : 0u;
      if (arg > CART_BLIT_MODE_BLITTER || piece > 255u) return 0;
      DPRINTF("dispatcher: host -> copy mode %u, piece %u\n", (unsigned)arg,
              (unsigned)piece);
      fb_set_copy_mode((uint8_t)arg, (uint8_t)piece);
      return 1;
    }
    case DEVHOOKS_APP_IKBD_CMD: {
      uint8_t cmd[CART_IKBD_OUT_MAX];
      size_t n = payloadSize / 2u;
      if (n > sizeof(cmd)) return 0;
      for (size_t i = 0; i < n; i++) cmd[i] = (uint8_t)payload[i];
      DPRINTF("dispatcher: host -> %u IKBD command byte(s)\n", (unsigned)n);
      return ikbd_send_commands(cmd, n) ? 1u : 0u;
    }
    default:
      return 0;
  }
}
#endif

void demo_dispatcher_render_frame(void) {
#if defined(_DEBUG) && (_DEBUG != 0)
  if (s_slow_frame_us != 0u) {
    busy_wait_us_32(s_slow_frame_us);
  }
#endif
  switch (s_state) {
    case DEMO_STATE_MENU:
      render_menu();
      break;
    case DEMO_STATE_ACTIVE:
      if (s_active && s_active->render_frame) {
        s_active->render_frame();
      }
      break;
    case DEMO_STATE_EXITING:
      /* Don't touch the framebuffer; the m68k will see CMD_BOOT_GEM
       * on its next VBL poll and exit cleanly back to GEM. */
      break;
  }
}
