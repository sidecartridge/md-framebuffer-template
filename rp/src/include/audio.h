/**
 * File: audio.h
 * Description: The app's sound, through the YM or the DMA sound chip.
 *
 * Two outputs, picked at every ST boot (audio.c):
 *   - on an STE or a Mega STE, the DMA sound chip: 8-bit samples at
 *     12,517 Hz, no interrupt per sample, from a ring in ST RAM that the ST
 *     fills every VBL from the cart buffer;
 *   - elsewhere (or when the app prefers it: audio_prefer_ym()), the YM: the
 *     m68k Timer-B IRQ (target/atarist/src/userfw.s) plays (vA, vB) volume
 *     pairs from the cart buffer at 5,585 Hz, one slice per VBL.
 * Either way the RP writes ahead of what the ST plays from a timer
 * interrupt, so the sound never tears and never repeats, however long the
 * app takes to draw a frame: audio_render_frame() only keeps a small FIFO
 * topped up, from the main loop, with the app's audio.
 *
 * Latency and stalls: the main loop may go AUDIO_FIFO_SLICES VBLs (4:
 * 80 ms by default) between two calls to audio_render_frame() without the
 * sound noticing. A sample plays at most AUDIO_FIFO_SLICES + 2 VBLs
 * (120 ms) after the source produced it on the YM, and about 150 ms on
 * the DMA chip, whose ring stays a few VBLs ahead. An app that wants less
 * latency defines a smaller AUDIO_FIFO_SLICES (at least 1) when it builds
 * audio.c, and tolerates shorter stalls. When the FIFO runs dry the output
 * holds the last sample, and audioUnderruns counts it (readable over SWD).
 *
 * The app's audio is YM pairs or 8-bit PCM; either plays on both outputs,
 * converted by the RP (the two-channel table both ways, linear resampling):
 *
 *   1. audio_play_loop(data, bytes) / audio_play_yms_file(path) /
 *      audio_set_fill_callback(cb): YM pairs, 2 bytes per sample (vA, vB)
 *      at 5,585 Hz, as tools/wav_to_ym4.py --mode dual-ghost writes them.
 *      The YM plays them as they are; the DMA chip plays the 6-bit samples
 *      they stand for.
 *   2. audio_play_pcm_loop(pcm, samples, rate) /
 *      audio_set_pcm_callback(cb, rate): signed 8-bit PCM at any rate. The
 *      DMA chip plays it at 12,517 Hz (unchanged at that rate); the YM plays
 *      its upper 6 bits through the same table.
 *
 * With no source installed, audio_render_frame() only follows the ST, and
 * the output holds the last sample played (zero after boot = silence).
 * Calling audio_set_fill_callback(NULL) re-enters this silent state.
 */

#ifndef AUDIO_H_INCLUDED
#define AUDIO_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "profile.h"

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

/* The DMA chip's rate, and the signed 8-bit sample callback: write exactly
 * `samples` samples into `buf` (the library asks for a few dozen at a
 * time). Called from audio_render_frame(), as for audio_fill_cb_t. */
#define AUDIO_DMA_RATE_HZ PROFILE_DMA_RATE_HZ /* 12,517, or 25,033 Hz at 25 fps */
typedef void (*audio_pcm_cb_t)(int8_t *buf, uint32_t samples);

/* Install a PCM source at `rate_hz` (AUDIO_DMA_RATE_HZ plays unchanged on
 * the DMA chip); replaces any other source. */
void audio_set_pcm_callback(audio_pcm_cb_t cb, uint32_t rate_hz);

/* Loop `samples` signed 8-bit samples at `rate_hz`; `pcm` must stay live
 * while it plays. Replaces any other source. */
void audio_play_pcm_loop(const int8_t *pcm, uint32_t samples, uint32_t rate_hz);

/* Keep to the YM even on a machine with the DMA chip, from the next ST
 * boot (the ST picks its output when it boots). Default: false. */
void audio_prefer_ym(bool prefer);

/* True when the ST plays through the DMA chip (as of its last boot). */
bool audio_uses_dma(void);

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
