/**
 * File: audio.h
 * Description: Cart-shared audio buffer producer (single-channel
 *              YM2149 ch A 4-bit DAC).
 *
 * The m68k Timer-B IRQ handler (target/atarist/src/userfw.s) fires
 * at ~6.27 kHz and reads one byte per fire from a 256-byte cart
 * buffer at CART_AUDIO_BUFFER_OFFSET. Each byte is a YM volume
 * nibble (0..15) in its low 4 bits.
 *
 * RP side: audio_init() builds a logarithmic LUT mapping linear
 * 8-bit PCM (0..255) to the closest-matching YM volume nibble.
 * audio_render_frame() generates samples (currently a simple sine)
 * and writes them through the LUT into the cart buffer.
 */

#ifndef AUDIO_H_INCLUDED
#define AUDIO_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_render_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H_INCLUDED */
