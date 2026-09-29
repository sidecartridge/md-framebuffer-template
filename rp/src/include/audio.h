/**
 * File: audio.h
 * Description: Cart-shared audio buffer producer + app-facing API.
 *
 * The m68k Timer-B IRQ (target/atarist/src/userfw.s) reads sample
 * bytes from the cart buffer at CART_AUDIO_BUFFER_OFFSET, one slice per
 * VBL, and writes them to YM2149 volume registers. The RP writes the
 * slices ahead of the one playing from a timer interrupt, so the sound
 * never tears and never repeats, however long the app takes to draw a
 * frame: audio_render_frame() only keeps a small FIFO topped up, from the
 * main loop, with the app's audio (audio.c).
 *
 * Latency: a sample plays at most AUDIO_FIFO_SLICES + 2 VBLs (120 ms by
 * default) after the callback produced it; the main loop may stall as
 * long without the sound noticing. An app that wants less latency defines
 * a smaller AUDIO_FIFO_SLICES (at least 1) when it builds audio.c. When
 * the FIFO runs dry the ST holds the last sample, and audioUnderruns
 * counts it (readable over SWD).
 *
 * Apps install audio content one of two ways:
 *
 *   1. audio_play_loop(data, bytes) -- convenience wrapper. The
 *      library installs a built-in callback that loops the given
 *      static buffer indefinitely. Typical use case for a baked-in
 *      jingle or sound effect.
 *
 *   2. audio_set_fill_callback(cb) -- low-level. The library
 *      invokes `cb(buf, bytes)` once per VBL of samples it needs,
 *      with `bytes` set to the m68k's per-VBL consumption; the
 *      callback writes exactly that many bytes into `buf`. Use for
 *      streaming sources (e.g. SD-backed PCM).
 *
 * Sample format is whatever the m68k Timer-B handler expects --
 * the default handler in userfw.s reads 2 bytes per sample
 * (vA, vB) in dual-channel Ghostbusters-LUT mode. The library is
 * format-agnostic; it just copies bytes into the cart buffer.
 *
 * If no callback is installed, audio_render_frame() is a no-op and
 * the slices hold the last sample played (zero after boot = silence).
 * Calling audio_set_fill_callback(NULL) re-enters this silent state.
 */

#ifndef AUDIO_H_INCLUDED
#define AUDIO_H_INCLUDED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-VBL fill callback. The library invokes this from
 * audio_render_frame() with `buf` pointing into its FIFO and `bytes`
 * set to the m68k's per-VBL consumption (currently 224 = 112 samples *
 * 2 B/sample at Timer-B 5,585 Hz). The callback must write exactly that
 * many bytes; the library does not zero on entry.
 *
 * Called from the main loop, as many times as the FIFO has room (once per
 * VBL on average); it may read the SD card, and must not block for long. */
typedef void (*audio_fill_cb_t)(uint8_t *buf, uint32_t bytes);

/* Initialise the cart audio buffer pointer; clear any previously
 * installed callback. Call once during boot. */
void audio_init(void);

/* Top up the FIFO through the fill callback (if one is installed). Call
 * once per main-loop iteration; more often does no harm. The slices
 * themselves are written from a timer interrupt, on the ST's VBLs. */
void audio_render_frame(void);

/* Install (or clear, if cb == NULL) the fill callback. */
void audio_set_fill_callback(audio_fill_cb_t cb);

/* Convenience: register a built-in callback that loops a static
 * byte buffer indefinitely. `data` must remain live for as long
 * as playback continues (typically a `static const uint8_t[]`
 * baked into the firmware -- e.g. audio_sample_data[] generated
 * by tools/wav_to_ym4.py). `bytes` is the total length of one
 * loop iteration; playback wraps at byte `bytes-1` back to byte 0.
 *
 * Replaces any previously installed callback. */
void audio_play_loop(const uint8_t *data, uint32_t bytes);

/* Convenience: open a .YMS file from SD and stream it on loop.
 * The file format (produced by `tools/wav_to_ym4.py --yms-output`)
 * is a 16-byte header followed by the raw byte body:
 *   off  0:  'Y' 'M' 'S' '1'        magic
 *   off  4:  uint32 rate_hz         little-endian; must match the
 *                                   m68k Timer-B rate
 *   off  8:  uint32 data_len_bytes  little-endian
 *   off 12:  uint8  mode_tag        1 = dual-ghost (only mode the
 *                                       current m68k handler plays)
 *   off 13:  uint8[3]              reserved (= 0)
 *   off 16:  raw byte body         streamed to the cart buffer
 *
 * The library opens the file, validates the header, and installs a
 * callback that reads one VBL of samples from the file at a time. SD is
 * assumed mounted; call after sdcard_initFilesystem(). The file
 * loops at EOF (cursor wraps back to the data start).
 *
 * Returns 0 on success, negative on failure (open failed / short
 * read / bad magic / rate mismatch / unsupported mode). On failure
 * the previously installed callback is preserved -- apps can use
 * this to fall back to a baked-in audio_play_loop(). */
int audio_play_yms_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H_INCLUDED */
