/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * plmarc.h - a movie .arc through pl_mpeg, behind one small interface
 *
 * Android only: the desktop builds keep ffmpeg (src/sys/moviefx.c) and
 * never include this header, so their preprocessed output is unchanged.
 *
 * src/sys/plmarc.c is the one translation unit that defines
 * PL_MPEG_IMPLEMENTATION (third_party/pl_mpeg, MIT).  It also owns the one
 * callback source that skips the 144-byte BURIKO ARC20 header in front of
 * the MPEG-PS stream; the player (src/sys/moviefx.c) opens its files
 * through PlmArc_Open, so the offset arithmetic lives in one place.
 *
 * The stream is MPEG-1 video + MP2 audio in an MPEG-PS container; the
 * pictures are decoded to YCrCb planes and converted on demand, the sound
 * to 1152 stereo frames of float per call.
 */
#ifndef BGI_PLMARC_H_
#define BGI_PLMARC_H_

#ifdef __ANDROID__

#include <stddef.h>
#include <stdint.h>

#define PLMARC_HEADER 144 // the BURIKO ARC20 header in front of the stream

typedef struct PlmArc_t PlmArc_t;

/* Open `path` (a native path) and read the stream headers.  NULL when the
 * file cannot be read, is shorter than the header, or carries no MPEG-PS
 * video stream. */
PlmArc_t* PlmArc_Open(const char* path);
void PlmArc_Close(PlmArc_t* a);

int PlmArc_Width(PlmArc_t* a);      // display size of the picture
int PlmArc_Height(PlmArc_t* a);
double PlmArc_Fps(PlmArc_t* a);     // 0 when unknown
int PlmArc_SampleRate(PlmArc_t* a); // 0 when there is no audio stream
int PlmArc_HasAudio(PlmArc_t* a);
double PlmArc_LengthMs(PlmArc_t* a); // 0 when it could not be worked out

/* Decode the next picture; 0 at the end of the stream.  The decoded frame
 * is valid until the next PlmArc_DecodeVideo. */
int PlmArc_DecodeVideo(PlmArc_t* a);
/* The frame PlmArc_DecodeVideo returned, as BGRA, `stride` bytes per row
 * (width * 4 for a tight image). */
void PlmArc_ConvertBgra(PlmArc_t* a, uint8_t* dst, int stride);

/* Decode the next audio frame (1152 stereo samples, interleaved float in
 * -1 .. 1); NULL at the end of the stream or when there is no audio.
 * *count receives the number of stereo frames. */
const float* PlmArc_DecodeAudio(PlmArc_t* a, unsigned* count);

double PlmArc_VideoTime(PlmArc_t* a); // stream time of the picture decoded last
double PlmArc_AudioTime(PlmArc_t* a);
int PlmArc_HasEnded(PlmArc_t* a);

#endif /* __ANDROID__ */

#endif /* BGI_PLMARC_H_ */
