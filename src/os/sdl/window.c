/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the SDL back end's main window and presentation (inc/bgi/os.h)
 *
 * The engine hands this layer a back buffer of plain memory and the
 * rectangles that changed (OS_BlitToWindow / OS_StretchToWindow).  They are
 * copied into a streaming texture of the picture's size, which the renderer
 * blits to the window and scales with SDL_RenderSetLogicalSize: the windowed
 * client area of another size ("81 64" of 1.529 on) and full screen both
 * show the picture centred with its aspect kept, black bars around it, and
 * the pointer coordinates are mapped back through the same transform (sys.c).
 * The picture is the logical size of the renderer, so this file only has to
 * put the engine's rectangles where the engine says they go.
 *
 * Full screen does not switch the display mode: the window covers the
 * desktop (SDL_WINDOW_FULLSCREEN_DESKTOP) and the mode the engine asked for
 * is scaled onto it, as in the POSIX back end.  OS_DisplaySetMode only
 * remembers the mode; the scaling itself is the renderer's.
 *
 * Not reproduced yet: the pop-up child windows ("B0 1x", unsupported.c), the
 * text entry ("B0 2x", unsupported.c) and the debugger's own window (it is
 * told there is none and draws over this one).
 */
#include "sdl_internal.h"

#include <string.h>

#if defined(__ANDROID__)
#include "bgi/android_prof.h"
#endif

SDL_Window* gSdlWin;                 // 0 before OS_WindowCreate and after OS_WindowDestroy
SDL_Renderer* gSdlRen;               // the renderer of that window
SdlDc_t gSdlMainDc = {-1};           // what OS_WindowGetDc hands out
int gSdlAlive, gSdlShown;            // created / shown
int gSdlActive, gSdlMinimized;       // the focus and iconic state (events.c keeps them)
int gSdlFullscreen;                  // the window covers the desktop
int gSdlClientW = 800, gSdlClientH = 600; // the window's client size
int gSdlPicW = 800, gSdlPicH = 600;       // the picture (the renderer's logical size)
int gSdlCloseRequested, gSdlQuitPosted;   // OS_WindowClose / OS_PostQuit, delivered by the next pump
static int gSdlVideoUp;                   // SDL_INIT_VIDEO was initialised

static SDL_Texture* gTex;  // the picture, in the engine's pixel format
static int gTexW, gTexH;   // its size (the picture's)
static Uint32 gTexFormat;  // SDL_PIXELFORMAT_ARGB8888 or ARGB1555
static uint8_t* gScratch;  // the stretch path's row buffer, grown as needed and kept
static size_t gScratchSize;

#if defined(__ANDROID__)
/* the window, renderer output, logical size, viewport and scale, for bgi.log */
static void Sdl_LogGeometry(const char* why)
{
	SDL_version v;
	int ww = 0, wh = 0, ow = 0, oh = 0, lw = 0, lh = 0;
	SDL_Rect vp;
	float sx = 0.0f, sy = 0.0f;
	SDL_GetVersion(&v);
	if(gSdlWin)
		SDL_GetWindowSize(gSdlWin, &ww, &wh);
	SDL_zero(vp);
	if(gSdlRen)
	{
		SDL_GetRendererOutputSize(gSdlRen, &ow, &oh);
		SDL_RenderGetLogicalSize(gSdlRen, &lw, &lh);
		SDL_RenderGetViewport(gSdlRen, &vp);
		SDL_RenderGetScale(gSdlRen, &sx, &sy);
	}
	fprintf(stderr,
		"sdl: android: geometry (%s): window %dx%d, output %dx%d, logical %dx%d, "
		"viewport %d,%d %dx%d, scale %.4f,%.4f, picture %dx%d, client %dx%d, "
		"fullscreen %d, sdl %d.%d.%d\n",
		why, ww, wh, ow, oh, lw, lh, vp.x, vp.y, vp.w, vp.h, sx, sy,
		gSdlPicW, gSdlPicH, gSdlClientW, gSdlClientH, gSdlFullscreen,
		v.major, v.minor, v.patch);
	fflush(stderr);
}
#endif

// ---- the video subsystem ------------------------------------------------------------------------

int Sdl_EnsureVideo(void)
{
	if(gSdlVideoUp)
		return 1;
	if(SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		fprintf(stderr, "sdl: SDL_Init(SDL_INIT_VIDEO) failed: %s\n", SDL_GetError());
		return 0;
	}
	gSdlVideoUp = 1;
	return 1;
}

void Sdl_ShutdownVideo(void)
{
	if(!gSdlVideoUp)
		return;
	SDL_Quit();
	gSdlVideoUp = 0;
}

// ---- the picture texture ------------------------------------------------------------------------

static void DropTexture(void)
{
	if(gTex)
	{
		SDL_DestroyTexture(gTex);
		gTex = NULL;
	}
	gTexW = gTexH = 0;
	gTexFormat = 0;
}

/* the texture of the picture, in the engine's pixel format: 32-bit
 * 0x00RRGGBB as ARGB8888 (the alpha byte is not looked at: the texture is
 * copied without blending), 16-bit RGB555 as ARGB1555.  Recreated when the
 * picture or the format changes; a new streaming texture is blacked first
 * because its contents are undefined. */
static int EnsureTexture(int bpp)
{
	Uint32 fmt = bpp == 16 ? SDL_PIXELFORMAT_ARGB1555 : SDL_PIXELFORMAT_ARGB8888;
	int bytes = bpp == 16 ? 2 : 4;
	void* zero;
	if(!gSdlRen || gSdlPicW <= 0 || gSdlPicH <= 0)
		return 0;
	if(gTex && gTexW == gSdlPicW && gTexH == gSdlPicH && gTexFormat == fmt)
		return 1;
	DropTexture();
	gTex = SDL_CreateTexture(gSdlRen, fmt, SDL_TEXTUREACCESS_STREAMING, gSdlPicW, gSdlPicH);
	if(!gTex)
	{
		fprintf(stderr, "sdl: SDL_CreateTexture(%dx%d) failed: %s\n", gSdlPicW, gSdlPicH, SDL_GetError());
		return 0;
	}
	SDL_SetTextureBlendMode(gTex, SDL_BLENDMODE_NONE);
	gTexW = gSdlPicW;
	gTexH = gSdlPicH;
	gTexFormat = fmt;
	zero = calloc((size_t)gSdlPicW * (size_t)gSdlPicH, (size_t)bytes);
	if(zero)
	{
		SDL_UpdateTexture(gTex, NULL, zero, gSdlPicW * bytes);
		free(zero);
	}
	return 1;
}

// a buffer of at least `bytes`, grown as needed and kept; NULL when out of memory
static void* Scratch(size_t bytes)
{
	if(bytes > gScratchSize)
	{
		uint8_t* p = (uint8_t*)realloc(gScratch, bytes);
		if(!p)
			return NULL;
		gScratch = p;
		gScratchSize = bytes;
	}
	return gScratch;
}

// one source pixel of a surface as 0x00RRGGBB (16-bit surfaces are RGB555, expanded to 8 bits per channel)
static uint32_t SurfacePixel(const OsSurface_t* s, int x, int y)
{
	const uint8_t* row = (const uint8_t*)s->pixels + (ptrdiff_t)y * s->pitch;
	if(s->bpp == 16)
	{
		uint32_t v = ((const uint16_t*)row)[x];
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

/* The workhorse behind OS_BlitToWindow / OS_StretchToWindow: the
 * (srcX, srcY, srcW, srcH) rectangle of `s` at (dstX, dstY) with the size
 * dstW x dstH, in picture pixels, clipped to the picture and to `clip`
 * ({l, t, r, b}, exclusive, or NULL).  A rectangle of the source's own size
 * is uploaded row by row; a stretched one is resampled nearest-neighbour
 * (the engine's own display scaling already happens above this layer, so
 * this only covers the "90 F1"-style whole-picture stretch), with black
 * where the source rectangle reaches outside the surface.  The result is
 * copied into the picture texture and presented. */
static void Blit(const SdlDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s, int srcX, int srcY,
	int srcW, int srcH, const int* clip)
{
	int l = 0, t = 0, r = gSdlPicW, b = gSdlPicH; // the destination, picture pixels
	int w, h, bytes;
	int sx0, sy0, direct;
	SDL_Rect dst;
#if defined(__ANDROID__)
	uint64_t t0 = AndroidProf_Us();
#endif

	if(!gSdlAlive || !gSdlRen || !s || !s->pixels || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0)
		return;
	if(s->bpp != 16 && s->bpp != 32)
		return;
	if(dc && dc->child >= 0)
		return; // the child windows have no surface of their own yet (unsupported.c)
	if(!EnsureTexture(s->bpp))
		return;
	if(clip)
	{
		if(clip[0] > l)
			l = clip[0];
		if(clip[1] > t)
			t = clip[1];
		if(clip[2] < r)
			r = clip[2];
		if(clip[3] < b)
			b = clip[3];
	}
	if(dstX > l)
		l = dstX;
	if(dstY > t)
		t = dstY;
	if(dstX + dstW < r)
		r = dstX + dstW;
	if(dstY + dstH < b)
		b = dstY + dstH;
	if(r <= l || b <= t)
		return;
	w = r - l;
	h = b - t;
	bytes = s->bpp == 16 ? 2 : 4;
	// the source rectangle of the visible part, when it lies inside the surface
	sx0 = srcX + (int)((int64_t)(l - dstX) * srcW / dstW);
	sy0 = srcY + (int)((int64_t)(t - dstY) * srcH / dstH);
	direct = dstW == srcW && dstH == srcH && sx0 >= 0 && sy0 >= 0 && sx0 + w <= s->width && sy0 + h <= s->height;
	dst.x = l;
	dst.y = t;
	dst.w = w;
	dst.h = h;
	if(direct)
	{ // 1:1: the rows go as they are
		const uint8_t* p = (const uint8_t*)s->pixels + (ptrdiff_t)sy0 * s->pitch + (ptrdiff_t)sx0 * bytes;
		if(SDL_UpdateTexture(gTex, &dst, p, s->pitch) != 0)
			return;
	}
	else
	{ // stretched: nearest neighbour, the source rectangle's pixels or black
		double fx = (double)srcW / dstW, fy = (double)srcH / dstH;
		uint8_t* buf = (uint8_t*)Scratch((size_t)w * (size_t)h * (size_t)bytes);
		int x, y;
		if(!buf)
			return;
		for(y = 0; y < h; y++)
		{
			int sy = srcY + (int)((t - dstY + y) * fy);
			if(sy >= srcY + srcH)
				sy = srcY + srcH - 1;
			for(x = 0; x < w; x++)
			{
				int sx = srcX + (int)((l - dstX + x) * fx);
				uint32_t rgb = 0;
				if(sx >= srcX + srcW)
					sx = srcX + srcW - 1;
				if(sx >= 0 && sx < s->width && sy >= 0 && sy < s->height)
					rgb = SurfacePixel(s, sx, sy);
				if(s->bpp == 16)
					((uint16_t*)buf)[(size_t)y * w + x] = (uint16_t)(0x8000u | ((rgb >> 19) & 0x1fu) << 10 | ((rgb >> 11) & 0x1fu) << 5 | ((rgb >> 3) & 0x1fu));
				else
					((uint32_t*)buf)[(size_t)y * w + x] = rgb;
			}
		}
		if(SDL_UpdateTexture(gTex, &dst, buf, w * bytes) != 0)
			return;
	}
#if defined(__ANDROID__)
	/* A GL ES back buffer is undefined after eglSwapBuffers, so every present
	 * redraws the whole texture rather than only the dirty rectangle. */
	SDL_RenderClear(gSdlRen);
	SDL_RenderCopy(gSdlRen, gTex, NULL, NULL);
	Sdl_DrawTouchBar(gSdlRen); // the corner button of the in-game row, over the picture
#else
	SDL_RenderCopy(gSdlRen, gTex, &dst, &dst); // the same rectangle: the picture is the logical size
#endif
	SDL_RenderPresent(gSdlRen);
#if defined(__ANDROID__)
	{
		static uint64_t lastUs;
		static unsigned count;
		uint64_t now = AndroidProf_Us();
		uint64_t gap = lastUs ? now - lastUs : 0;
		uint64_t dt = now - t0;
		count++;
		lastUs = now;
		if(count == 1 || gap >= 50000 || dt >= 20000)
			AndroidProf_Log("blit #%u %dx%d gap %llu ms, update+present %llu us%s", count, w, h,
				(unsigned long long)(gap / 1000), (unsigned long long)dt, gap >= 50000 ? " GAP" : "");
	}
#endif
}

// ---- the device context -------------------------------------------------------------------------

// the "device context" is a token naming the window; nothing is acquired, so there is nothing to release
OsDc_t* OS_WindowGetDc(void)
{
	return gSdlAlive ? &gSdlMainDc : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	BGI_UNUSED(dc);
}

// BitBlt: an unscaled copy of w x h pixels
void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	Blit(dc ? (const SdlDc_t*)dc : &gSdlMainDc, dstX, dstY, w, h, src, srcX, srcY, w, h, NULL);
}

// StretchDIBits of the whole surface; a clip rectangle also bounds the output
void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, const int* clip)
{
	if(!src)
		return;
	Blit(dc ? (const SdlDc_t*)dc : &gSdlMainDc, dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height, clip);
}

// the whole window black (the `paint` handler before the engine is up)
void Sdl_FillBlack(void)
{
	if(!gSdlRen)
		return;
	SDL_SetRenderDrawColor(gSdlRen, 0, 0, 0, 255);
	SDL_RenderClear(gSdlRen);
	SDL_RenderPresent(gSdlRen);
}

void OS_WindowFillBlack(void)
{
	Sdl_FillBlack();
}

// ---- geometry -----------------------------------------------------------------------------------

void Sdl_SyncClientSize(void)
{
	int w = 0, h = 0;
	if(!gSdlWin)
		return;
	SDL_GetWindowSize(gSdlWin, &w, &h);
	if(w > 0 && h > 0)
	{
		gSdlClientW = w;
		gSdlClientH = h;
	}
}

/* the picture as the logical size of the renderer: SDL does the scaling and
 * keeps the aspect, letterboxing the rest of the window, and redoes it on
 * every resize; the window is blacked so that nothing of the old picture is
 * left around it */
#if defined(__ANDROID__)
static int gSdlLogicalW, gSdlLogicalH;   // the logical size applied last
static int gSdlLogicalCW, gSdlLogicalCH; // and the client size it was applied for
#endif

void Sdl_ApplyLogical(void)
{
	if(!gSdlRen)
		return;
#if defined(__ANDROID__)
	/* one apply per surface state: repeated window events would otherwise
	 * black the window again for the same logical size */
	if(gSdlLogicalW == gSdlPicW && gSdlLogicalH == gSdlPicH &&
		gSdlLogicalCW == gSdlClientW && gSdlLogicalCH == gSdlClientH)
	{
		Sdl_LogGeometry("logical size unchanged"); // the cache skipped the apply
		return;
	}
	gSdlLogicalW = gSdlPicW;
	gSdlLogicalH = gSdlPicH;
	gSdlLogicalCW = gSdlClientW;
	gSdlLogicalCH = gSdlClientH;
#endif
	SDL_RenderSetLogicalSize(gSdlRen, gSdlPicW, gSdlPicH);
	Sdl_FillBlack();
#if defined(__ANDROID__)
	Sdl_LogGeometry("logical size applied");
#endif
}

// a float to an int, rounding to the nearer one on both sides of zero
static int RoundF(float v)
{
	return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

void Sdl_WindowToPicture(float wx, float wy, int* px, int* py)
{
	float lx = wx, ly = wy;
	if(gSdlRen)
		SDL_RenderWindowToLogical(gSdlRen, (int)wx, (int)wy, &lx, &ly);
	*px = RoundF(lx);
	*py = RoundF(ly);
#if defined(__ANDROID__)
	/* a point in the letterbox around the picture is not a picture
	 * coordinate: clamp a tap there to the nearest picture pixel */
	if(*px < 0)
		*px = 0;
	else if(*px >= gSdlPicW)
		*px = gSdlPicW - 1;
	if(*py < 0)
		*py = 0;
	else if(*py >= gSdlPicH)
		*py = gSdlPicH - 1;
#endif
}

void Sdl_PictureToWindow(float px, float py, int* wx, int* wy)
{
	int x = RoundF(px), y = RoundF(py);
	if(gSdlRen)
		SDL_RenderLogicalToWindow(gSdlRen, px, py, &x, &y);
	*wx = x;
	*wy = y;
}

// GetWindowRect: the frame's rectangle on the screen; in full screen the picture at (0, 0)
void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	int fw = 0, fh = 0;
	if(!gSdlWin)
	{
		*x = *y = 0;
		*w = gSdlClientW;
		*h = gSdlClientH;
		return;
	}
	if(gSdlFullscreen)
	{
		*x = *y = 0;
		*w = gSdlPicW;
		*h = gSdlPicH;
		return;
	}
	SDL_GetWindowPosition(gSdlWin, x, y);
	OS_FrameMetrics(&fw, &fh);
	*w = gSdlClientW + fw;
	*h = gSdlClientH + fh;
}

// ---- creation -----------------------------------------------------------------------------------

/* Create the main window, hidden: the client size the engine asked for, the
 * title as OsCommon_MainTitle spells it, a renderer with the picture as its
 * logical size and the arrow cursor.  Raises `created`.  0 without a video
 * device or when a window exists already. */
int OS_WindowCreate(const OsWindowDesc_t* d)
{
	char title[0x400], utf8[0x400];
	Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;
	int x, y;
	if(!d || gSdlWin)
		return 0;
	if(!Sdl_EnsureVideo())
		return 0;
	OS_ScreenSize(&gSdlScreenW, &gSdlScreenH);
	gSdlClientW = d->width > 0 ? d->width : 800;
	gSdlClientH = d->height > 0 ? d->height : 600;
	gSdlPicW = gSdlClientW;
	gSdlPicH = gSdlClientH;
	OsCommon_MainTitle(title, sizeof title, d->title);
	OsPosix_SjisToUtf8(title, utf8, sizeof utf8);
	x = d->x;
	y = d->y;
	if(x < -32000 || y < -32000) // a nonsense position (negative magic values): let the window manager place it
		x = y = SDL_WINDOWPOS_UNDEFINED;
#if defined(__ANDROID__)
	/* A phone has one window, always full screen: the size the engine asks
	 * for is only the picture size, which the renderer scales onto the
	 * display with the aspect kept. */
	flags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI;
	x = y = SDL_WINDOWPOS_UNDEFINED;
#endif
	gSdlWin = SDL_CreateWindow(utf8, x, y, gSdlClientW, gSdlClientH, flags);
	if(!gSdlWin)
	{
		fprintf(stderr, "sdl: SDL_CreateWindow failed: %s\n", SDL_GetError());
		return 0;
	}
	// bilinear when the picture is scaled into the window (the engine draws at the picture's size)
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	gSdlRen = SDL_CreateRenderer(gSdlWin, -1, SDL_RENDERER_ACCELERATED);
	if(!gSdlRen)
		gSdlRen = SDL_CreateRenderer(gSdlWin, -1, 0); // software
	if(!gSdlRen)
	{
		fprintf(stderr, "sdl: SDL_CreateRenderer failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(gSdlWin);
		gSdlWin = NULL;
		return 0;
	}
	SDL_SetRenderDrawColor(gSdlRen, 0, 0, 0, 255);
	gSdlMainDc.child = -1;
	gSdlAlive = 1;
	gSdlShown = 0;
	gSdlActive = 0;
	gSdlMinimized = 0;
#if defined(__ANDROID__)
	gSdlFullscreen = 1; // the window covers the display, whatever the engine switches to
#else
	gSdlFullscreen = 0;
#endif
	gSdlCloseRequested = gSdlQuitPosted = 0;
	memset(gSdlKeys, 0, sizeof gSdlKeys);
	Sdl_CursorApply();
#if defined(__ANDROID__)
	Sdl_SyncClientSize(); // the full-screen size SDL really got, in points
#endif
	Sdl_ApplyLogical();
	if(gSdlHandlers.created)
		gSdlHandlers.created();
	return 1;
}

/* DestroyWindow: the texture, the renderer and the window go, then SDL
 * itself (nothing else in the engine draws after this); `destroyed` is
 * raised before the return, as WM_DESTROY is */
void OS_WindowDestroy(void)
{
	if(!gSdlWin)
		return;
	DropTexture();
	if(gSdlRen)
	{
		SDL_DestroyRenderer(gSdlRen);
		gSdlRen = NULL;
	}
	SDL_DestroyWindow(gSdlWin);
	gSdlWin = NULL;
	Sdl_ShutdownVideo();
	gSdlAlive = 0;
	gSdlShown = 0;
	gSdlActive = 0;
	gSdlMinimized = 0;
	gSdlFullscreen = 0;
	if(gSdlHandlers.destroyed)
		gSdlHandlers.destroyed();
}

int OS_WindowAlive(void)
{
	return gSdlAlive;
}

// show (raised) or hide the window
void OS_WindowShow(int show)
{
	if(!gSdlWin)
		return;
	if(show)
	{
		SDL_ShowWindow(gSdlWin);
		SDL_RaiseWindow(gSdlWin);
		gSdlShown = 1;
		Sdl_SyncClientSize();
		Sdl_ApplyLogical();
	}
	else
	{
		SDL_HideWindow(gSdlWin);
		gSdlShown = 0;
	}
}

// the title with " - OpenBGI" appended; NULL leaves "OpenBGI" alone, as SetWindowTextA(NULL) clears the text
void OS_WindowSetTitle(const char* t)
{
	char title[0x400], utf8[0x400];
	if(!gSdlWin)
		return;
	OsCommon_MainTitle(title, sizeof title, t);
	OsPosix_SjisToUtf8(title, utf8, sizeof utf8);
	SDL_SetWindowTitle(gSdlWin, utf8);
}

// move the frame's corner to (x, y); ignored in full screen
void OS_WindowMove(int x, int y)
{
#if defined(__ANDROID__)
	/* the window covers the display: its position is not the engine's */
	BGI_UNUSED(x);
	BGI_UNUSED(y);
#else
	if(!gSdlWin || gSdlFullscreen)
		return;
	SDL_SetWindowPosition(gSdlWin, x, y);
#endif
}

int OS_WindowMinimized(void)
{
	return gSdlMinimized;
}

int OS_WindowActive(void)
{
	return gSdlActive;
}

// iconify; the window event SDL_WINDOWEVENT_MINIMIZED reports it
void OS_WindowMinimize(void)
{
	if(gSdlWin)
		SDL_MinimizeWindow(gSdlWin);
}

// like posting WM_CLOSE: `close_request` from the next pump
void OS_WindowClose(void)
{
	gSdlCloseRequested = 1;
}

/* SIGTERM / SIGINT (process.c): the quit the window manager's close would
 * post once the engine agreed to it (gSdlHandlers.quit), so that an
 * unattended run ends through the engine's own shutdown (and writes
 * BGI_OPSTAT).  Only a flag is set: the pump raises the event.  This runs in
 * a signal handler, so nothing of SDL may be touched here. */
void OsPosix_RequestClose(void)
{
	gSdlQuitPosted = 1;
}

// no IME status window to show or hide: SDL has no input method of its own
void OS_ImeStatus(int open)
{
	BGI_UNUSED(open);
}

// ---- display modes ------------------------------------------------------------------------------

/* no mode switch: the requested mode is remembered (window.c's scaling shows
 * the picture centred on the desktop) and the depth is ignored; 1 when the
 * size is usable */
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	BGI_UNUSED(fullscreen);
	BGI_UNUSED(bpp);
	if(w <= 0 || h <= 0)
		return 0;
	return 1;
}

// nothing was switched, so there is nothing to restore
void OS_DisplayRestore(void)
{
}

// the window over the whole desktop with the picture scaled onto it
void OS_WindowSetFullscreen(int modeW, int modeH)
{
	if(!gSdlWin)
		return;
	BGI_UNUSED(modeW);
	BGI_UNUSED(modeH);
	OS_ScreenSize(&gSdlScreenW, &gSdlScreenH);
	gSdlFullscreen = 1;
#if defined(__ANDROID__)
	/* the window is full screen already: only the picture's size changes
	 * (OS_WindowSetPicture), the size SDL was asked for is ignored */
#else
	SDL_SetWindowFullscreen(gSdlWin, SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif
	Sdl_SyncClientSize();
	Sdl_ApplyLogical();
}

// back to (or still) a framed window of clientW x clientH at the outer origin (x, y)
void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
#if defined(__ANDROID__)
	/* there is no windowed mode on a phone: the window keeps the display's
	 * size and the picture is scaled onto it, centred and letterboxed */
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(clientW);
	BGI_UNUSED(clientH);
	if(!gSdlWin)
		return;
	Sdl_SyncClientSize();
	Sdl_ApplyLogical();
#else
	if(!gSdlWin)
		return;
	if(gSdlFullscreen)
	{
		gSdlFullscreen = 0;
		SDL_SetWindowFullscreen(gSdlWin, 0);
	}
	gSdlClientW = clientW > 0 ? clientW : gSdlClientW;
	gSdlClientH = clientH > 0 ? clientH : gSdlClientH;
	SDL_SetWindowSize(gSdlWin, gSdlClientW, gSdlClientH);
	SDL_SetWindowPosition(gSdlWin, x, y);
	Sdl_SyncClientSize();
	Sdl_ApplyLogical();
#endif
}

// the picture the engine draws: the renderer's logical size (scaled into a client area of another size)
void OS_WindowSetPicture(int w, int h)
{
	if(w <= 0 || h <= 0)
		return;
	if(gSdlPicW == w && gSdlPicH == h)
		return;
	gSdlPicW = w;
	gSdlPicH = h;
	DropTexture(); // it is of the picture's size
	Sdl_ApplyLogical();
}
