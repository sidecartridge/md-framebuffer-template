/**
 * File: demo_input.c
 * Description: Input test: the keyboard, the mouse and both joysticks as the
 *              RP decodes them, and the counters of the IKBD path.
 *
 * F1..F4 pick the input mode (ikbd_set_input_mode()); the screen shows the
 * mode asked for and the one the ST reports, a pointer the mouse moves,
 * both buttons, both sticks, the last key, and how many clicks and fire
 * presses were seen (the "pressed" latches catch the short ones). The
 * bottom lines are the IKBD path's counters: bytes and packets decoded,
 * and every sign of a lost byte (ikbd_demux.h). Written against ikbd.h
 * only, as an app would be. Starts in mouse mode; leaving it puts the IKBD
 * back in keyboard mode.
 */

#include <stdint.h>

#include "demo.h"
#include "fb.h"
#include "fb_blit.h"
#include "fb_chunked.h"
#include "fb_font.h"
#include "ikbd.h"
#include "palette.h"

extern const struct FB_FONT font8x8;

#define COL_BG 0
#define COL_TEXT 1
#define COL_ON 2
#define COL_OFF 3
#define COL_POINTER 4
#define COL_LABEL 5
#define COL_KEY 15 /* the pointer's transparent pixels */

static const uint16_t s_palette[PALETTE_ENTRIES] = {
    PALETTE_RGB(0, 0, 1), PALETTE_RGB(7, 7, 7), PALETTE_RGB(1, 6, 1),
    PALETTE_RGB(2, 2, 3), PALETTE_RGB(7, 2, 1), PALETTE_RGB(6, 5, 2),
};

/* IKBD scancodes of the mode keys. */
#define SC_F1 0x3Bu
#define SC_F2 0x3Cu
#define SC_F3 0x3Du
#define SC_F4 0x3Eu

static const char *const k_mode_names[] = {"KEYS", "MOUSE", "MOUSE+JOY1",
                                           "JOYSTICKS"};

/* The pointer: an arrow, hot spot at its top left. */
#define _ COL_KEY
#define W COL_TEXT
#define R COL_POINTER
static const uint8_t k_arrow[10 * 7] = {
    W, _, _, _, _, _, _,  //
    W, W, _, _, _, _, _,  //
    W, R, W, _, _, _, _,  //
    W, R, R, W, _, _, _,  //
    W, R, R, R, W, _, _,  //
    W, R, R, R, R, W, _,  //
    W, R, R, W, W, W, W,  //
    W, W, W, R, W, _, _,  //
    _, _, _, W, R, W, _,  //
    _, _, _, _, W, W, _,  //
};
#undef _
#undef W
#undef R
static const struct FB_BITMAP k_arrow_bitmap = {7, 10, k_arrow};

static int s_px;
static int s_py;
static uint8_t s_buttons;
static uint8_t s_joy[2];
static uint32_t s_clicks[2]; /* left, right */
static uint32_t s_fires[2];  /* stick 0, stick 1 */
static uint32_t s_keys;
static uint8_t s_last_key;
static bool s_last_key_press;

static const char *num(uint32_t n, char *buf, int size) {
  char *p = buf + size - 1;
  *p = '\0';
  do {
    *--p = (char)('0' + (n % 10u));
    n /= 10u;
  } while (n != 0u && p > buf);
  return p;
}

static const char *hex2(uint8_t n, char *buf) {
  static const char k_hex[] = "0123456789ABCDEF";
  buf[0] = '$';
  buf[1] = k_hex[n >> 4];
  buf[2] = k_hex[n & 0x0Fu];
  buf[3] = '\0';
  return buf;
}

static void print_at(int x, int y, const char *text) {
  font_move((unsigned)x, (unsigned)y);
  font_print(text);
}

static void print_count(int x, int y, const char *label, uint32_t value) {
  char buf[11];
  font_set_color(COL_LABEL);
  print_at(x, y, label);
  font_set_color(COL_TEXT);
  font_print(num(value, buf, sizeof(buf)));
}

static void box(int x, int y, int w, int h, bool on) {
  fb_fill_rect(x, y, w, h, on ? COL_ON : COL_OFF);
}

/* One stick: four direction boxes around the fire box. */
static void draw_stick(int cx, int cy, unsigned port) {
  char label[] = "JOY 0";
  label[4] = (char)('0' + port);
  font_set_color(COL_LABEL);
  print_at(cx - 20, cy - 36, label);
  uint8_t s = s_joy[port];
  box(cx - 7, cy - 25, 14, 14, s & IKBD_JOY_UP);
  box(cx - 7, cy + 11, 14, 14, s & IKBD_JOY_DOWN);
  box(cx - 25, cy - 7, 14, 14, s & IKBD_JOY_LEFT);
  box(cx + 11, cy - 7, 14, 14, s & IKBD_JOY_RIGHT);
  box(cx - 7, cy - 7, 14, 14, s & IKBD_JOY_FIRE);
  print_count(cx - 28, cy + 30, "FIRE ", s_fires[port]);
}

static void input_init(void) {
  palette_set(s_palette);
  s_px = FB_CHUNKED_W / 2;
  s_py = FB_CHUNKED_H / 2;
  s_buttons = 0;
  s_joy[0] = s_joy[1] = 0;
  s_clicks[0] = s_clicks[1] = 0;
  s_fires[0] = s_fires[1] = 0;
  s_keys = 0;
  s_last_key = 0;
  s_last_key_press = false;
  ikbd_set_input_mode(IKBD_INPUT_MOUSE);
}

static void input_handle_key(const ikbd_key_event_t *k) {
  if (k->is_press) {
    s_keys++;
    if (k->scancode >= SC_F1 && k->scancode <= SC_F4) {
      ikbd_set_input_mode((ikbd_input_mode_t)(k->scancode - SC_F1));
    }
  }
  s_last_key = k->scancode;
  s_last_key_press = k->is_press;
}

static void input_render_frame(void) {
  ikbd_mouse_t m;
  ikbd_read_mouse(&m);
  s_px += m.dx;
  s_py += m.dy;
  if (s_px < 0) s_px = 0;
  if (s_px > FB_CHUNKED_W - 1) s_px = FB_CHUNKED_W - 1;
  if (s_py < 0) s_py = 0;
  if (s_py > FB_CHUNKED_H - 1) s_py = FB_CHUNKED_H - 1;
  s_buttons = m.buttons;
  if (m.pressed & IKBD_MOUSE_LEFT) s_clicks[0]++;
  if (m.pressed & IKBD_MOUSE_RIGHT) s_clicks[1]++;
  for (unsigned port = 0; port < 2u; port++) {
    ikbd_joystick_t j;
    ikbd_read_joystick(port, &j);
    s_joy[port] = j.state;
    if (j.pressed & IKBD_JOY_FIRE) s_fires[port]++;
  }

  fb_chunked_clear(COL_BG);
  font_set_font(&font8x8);
  font_set_border(0, 0);
  font_align(FONT_ALIGN_LEFT);

  font_set_color(COL_TEXT);
  print_at(8, 4, "INPUT TEST");
  font_set_color(COL_LABEL);
  print_at(248, 4, "ESC=MENU");
  print_at(8, 16, "F1 KEYS F2 MOUSE F3 M+JOY1 F4 JOYS");

  int live = ikbd_get_live_input_mode();
  print_at(8, 30, "MODE ");
  font_set_color(COL_TEXT);
  font_print(k_mode_names[ikbd_get_input_mode()]);
  font_set_color(COL_LABEL);
  font_print("  ST ");
  font_set_color(live == (int)ikbd_get_input_mode() ? COL_ON : COL_POINTER);
  font_print(live < 0 ? "-" : k_mode_names[live]);

  char buf[11];
  font_set_color(COL_LABEL);
  print_at(8, 42, "KEY ");
  font_set_color(COL_TEXT);
  if (s_last_key != 0u) {
    font_print(hex2(s_last_key, buf));
    font_print(s_last_key_press ? " DOWN" : " UP");
  } else {
    font_print("-");
  }
  print_count(168, 42, "PRESSES ", s_keys);

  /* Mouse: position, buttons, clicks. */
  print_count(8, 58, "MOUSE X ", (uint32_t)s_px);
  print_count(128, 58, "Y ", (uint32_t)s_py);
  box(200, 56, 24, 12, s_buttons & IKBD_MOUSE_LEFT);
  box(228, 56, 24, 12, s_buttons & IKBD_MOUSE_RIGHT);
  print_count(8, 70, "CLICKS L ", s_clicks[0]);
  print_count(128, 70, "R ", s_clicks[1]);

  draw_stick(80, 118, 0);
  draw_stick(240, 118, 1);

  /* The IKBD path's counters (ikbd_demux.h). */
  const ikbd_demux_t *d = ikbd_demux();
  print_count(8, 162, "BYTES ", d->bytes);
  print_count(128, 162, "MOUSE ", d->mouse_packets);
  print_count(224, 162, "JOY ", d->joy_events + d->joy_reports);
  print_count(8, 174, "LOST ", ikbd_overruns());
  print_count(96, 174, "RESYNC ", ikbd_demux_resyncs(d));
  print_count(208, 174, "COUNT ", d->resyncs[IKBD_RESYNC_COUNT]);
  print_count(8, 186, "GUARDS ", d->guards);
  print_count(128, 186, "DROPPED ", d->dropped_presses);
  if (d->guard) {
    font_set_color(COL_POINTER);
    print_at(272, 186, "GUARD");
  }

  fb_blit_key(&k_arrow_bitmap, s_px, s_py, COL_KEY);
  fb_publish();
}

static void input_teardown(void) { ikbd_set_input_mode(IKBD_INPUT_KEYBOARD); }

const demo_module_t demo_input = {
    .name = "input test",
    .init = input_init,
    .render_frame = input_render_frame,
    .handle_key = input_handle_key,
    .teardown = input_teardown,
};
