/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * plmarc.c - the pl_mpeg decoder of inc/bgi/plmarc.h (Android only)
 *
 * This is the one translation unit with PL_MPEG_IMPLEMENTATION: pl_mpeg.h
 * defines its whole implementation under that macro, so a second one would
 * duplicate every symbol.  The callback source below is the only place that
 * knows about the 144-byte BURIKO ARC20 header: pl_mpeg's file buffer seeks
 * back to offset 0, so every position is made relative to the payload
 * instead.  The desktop builds keep ffmpeg and compile an empty unit.
 */
#if defined(__ANDROID__)

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include "bgi/plmarc.h"

typedef struct
{
	FILE* f;
	long base; // PLMARC_HEADER: the first byte of the MPEG-PS stream
} ArcSrc_t;

static void SrcLoad(plm_buffer_t* b, void* user)
{
	ArcSrc_t* s = (ArcSrc_t*)user;
	size_t room, got;
	plm_buffer_discard_read_bytes(b); // the consumed prefix (what the file mode does)
	room = b->capacity - b->length;
	got = fread(b->bytes + b->length, 1, room, s->f);
	b->length += got;
	if(got == 0)
		b->has_ended = 1;
}

static void SrcSeek(plm_buffer_t* b, size_t offset, void* user)
{
	ArcSrc_t* s = (ArcSrc_t*)user;
	(void)b;
	fseek(s->f, s->base + (long)offset, SEEK_SET);
}

static size_t SrcTell(plm_buffer_t* b, void* user)
{
	ArcSrc_t* s = (ArcSrc_t*)user;
	(void)b;
	return (size_t)(ftell(s->f) - s->base);
}

struct PlmArc_t
{
	ArcSrc_t src;
	plm_t* plm;
	plm_frame_t* frame; // the picture PlmArc_DecodeVideo returned last
};

PlmArc_t* PlmArc_Open(const char* path)
{
	PlmArc_t* a;
	plm_buffer_t* buf;
	long size;

	a = (PlmArc_t*)calloc(1, sizeof *a);
	if(!a)
		return NULL;
	a->src.f = fopen(path, "rb");
	if(!a->src.f)
	{
		free(a);
		return NULL;
	}
	fseek(a->src.f, 0, SEEK_END);
	size = ftell(a->src.f);
	fseek(a->src.f, 0, SEEK_SET);
	if(size <= PLMARC_HEADER)
	{
		fclose(a->src.f);
		free(a);
		return NULL;
	}
	a->src.base = PLMARC_HEADER;
	buf = plm_buffer_create_with_callbacks(SrcLoad, SrcSeek, SrcTell, (size_t)(size - PLMARC_HEADER), &a->src);
	a->plm = plm_create_with_buffer(buf, 1); // the plm_t owns the buffer from here (and frees it in plm_destroy)
	if(!a->plm || plm_get_width(a->plm) <= 0 || plm_get_height(a->plm) <= 0)
	{
		if(a->plm)
			plm_destroy(a->plm);
		else
			plm_buffer_destroy(buf);
		fclose(a->src.f);
		free(a);
		return NULL;
	}
	return a;
}

void PlmArc_Close(PlmArc_t* a)
{
	if(!a)
		return;
	plm_destroy(a->plm); // frees the buffer, not the FILE
	fclose(a->src.f);
	free(a);
}

int PlmArc_Width(PlmArc_t* a)
{
	return a ? plm_get_width(a->plm) : 0;
}

int PlmArc_Height(PlmArc_t* a)
{
	return a ? plm_get_height(a->plm) : 0;
}

double PlmArc_Fps(PlmArc_t* a)
{
	return a ? plm_get_framerate(a->plm) : 0.0;
}

int PlmArc_SampleRate(PlmArc_t* a)
{
	return a ? plm_get_samplerate(a->plm) : 0;
}

int PlmArc_HasAudio(PlmArc_t* a)
{
	return a && plm_get_num_audio_streams(a->plm) > 0;
}

/* The length the script is told ("90 F0").  pl_mpeg finds it by scanning
 * back from the end of the file for the last video PTS; the result is cached
 * after the first call. */
double PlmArc_LengthMs(PlmArc_t* a)
{
	double d;
	if(!a)
		return 0.0;
	d = plm_get_duration(a->plm);
	if(!(d > 0.0) || d > 86400.0)
		return 0.0;
	return d * 1000.0;
}

int PlmArc_DecodeVideo(PlmArc_t* a)
{
	if(!a)
		return 0;
	a->frame = plm_decode_video(a->plm);
	return a->frame != NULL;
}

void PlmArc_ConvertBgra(PlmArc_t* a, uint8_t* dst, int stride)
{
	if(a && dst && a->frame)
		plm_frame_to_bgra(a->frame, dst, stride);
}

const float* PlmArc_DecodeAudio(PlmArc_t* a, unsigned* count)
{
	plm_samples_t* s;
	if(count)
		*count = 0;
	if(!a)
		return NULL;
	s = plm_decode_audio(a->plm);
	if(!s)
		return NULL;
	if(count)
		*count = s->count;
	return s->interleaved;
}

double PlmArc_VideoTime(PlmArc_t* a)
{
	return a && a->plm->video_decoder ? plm_video_get_time(a->plm->video_decoder) : 0.0;
}

double PlmArc_AudioTime(PlmArc_t* a)
{
	return a && a->plm->audio_decoder ? plm_audio_get_time(a->plm->audio_decoder) : 0.0;
}

int PlmArc_HasEnded(PlmArc_t* a)
{
	return a ? plm_has_ended(a->plm) : 1;
}

#else

typedef int PlmArc_NotBuilt; // the decoder is Android-only: nothing to compile here

#endif
