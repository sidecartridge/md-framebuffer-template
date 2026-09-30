/**
 * File: profile.h
 * Description: The app's profile, chosen at compile time: how often the ST
 *              shows a new frame, and how fast it plays the sound.
 *
 *   PROFILE_50FPS (the default): a frame every VBL, 50 frames a second; the
 *     YM at 5,585 Hz, the DMA chip of an STE or a Mega STE at 12,517 Hz.
 *     For arcade games and effects that change the whole screen.
 *   PROFILE_25FPS: a frame every second VBL, 25 frames a second on every
 *     machine; the YM at 21,943 Hz, the DMA chip at 25,033 Hz. On a plain
 *     ST the full-screen copy and the sound take about 35 ms of the frame's
 *     40. For story games with rich sound (laserdisc-style).
 *
 * Choose it with APP_PROFILE: `APP_PROFILE=PROFILE_25FPS ./build.sh ...`
 * (rp/src/CMakeLists.txt passes it on), or a define before this header.
 * The ST's image is the same for both: the RP writes the profile into the
 * cartridge window (CART_PROFILE_OFFSET) before the ST boots, and userfw
 * reads it once. fb_publish() then paces the app at the profile's frame
 * rate. Sound sources keep their own rates: the RP resamples them.
 */

#ifndef PROFILE_H
#define PROFILE_H

#include "cart_shared.h"

#define PROFILE_50FPS 0
#define PROFILE_25FPS 1

#ifndef APP_PROFILE
#define APP_PROFILE PROFILE_50FPS
#endif

#if APP_PROFILE == PROFILE_25FPS
#define PROFILE_ID CART_PROFILE_25FPS
#define PROFILE_NAME "25 fps"
#define PROFILE_VBLS_A_FRAME CART_PROFILE_25FPS_VBLS
#define PROFILE_YM_RATE_HZ CART_PROFILE_25FPS_YM_RATE_HZ
#define PROFILE_YM_BYTES_PER_VBL CART_PROFILE_25FPS_YM_BYTES
#define PROFILE_DMA_RATE_HZ CART_PROFILE_25FPS_DMA_RATE_HZ
#define PROFILE_DMA_BYTES_PER_VBL CART_PROFILE_25FPS_DMA_BYTES
#define PROFILE_DMA_LEAD CART_PROFILE_25FPS_DMA_LEAD
#elif APP_PROFILE == PROFILE_50FPS
#define PROFILE_ID CART_PROFILE_50FPS
#define PROFILE_NAME "50 fps"
#define PROFILE_VBLS_A_FRAME CART_PROFILE_50FPS_VBLS
#define PROFILE_YM_RATE_HZ CART_PROFILE_50FPS_YM_RATE_HZ
#define PROFILE_YM_BYTES_PER_VBL CART_PROFILE_50FPS_YM_BYTES
#define PROFILE_DMA_RATE_HZ CART_PROFILE_50FPS_DMA_RATE_HZ
#define PROFILE_DMA_BYTES_PER_VBL CART_PROFILE_50FPS_DMA_BYTES
#define PROFILE_DMA_LEAD CART_PROFILE_50FPS_DMA_LEAD
#else
#error "APP_PROFILE must be PROFILE_50FPS or PROFILE_25FPS"
#endif

#endif /* PROFILE_H */
