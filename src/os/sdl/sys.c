/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sys.c - the SDL back end's screen, display-mode, cursor and key state
 *         services (inc/bgi/os.h)
 *
 * What the window itself does not need: the desktop's size, the frame the
 * window manager put around the window (SDL_GetWindowBordersSize), the
 * "display mode" (there is none to switch: the picture is scaled onto the
 * desktop, see window.c), the five cursors of "80 67" through SDL's system
 * cursors, and the key and button state OS_KeyState reports, kept by the
 * event pump (events.c) in gSdlKeys.
 *
 * The pointer lives in three spaces: the screen, the window's client area,
 * and the picture.  Everything above this layer works in picture pixels (the
 * renderer's logical size), so the two conversions below take the client
 * origin from SDL and the scale from the renderer, exactly as the blits of
 * window.c do.
 */
#include "sdl_internal.h"

int gSdlScreenW = 1024, gSdlScreenH = 768; // the desktop; 1024 x 768 before SDL answers
uint8_t gSdlKeys[256];                     // GetAsyncKeyState emulation, indexed by virtual key

// ---- the screen ---------------------------------------------------------------------------------

// the desktop in pixels (also refreshes the cached size)
void OS_ScreenSize(int* w, int* h)
{
	SDL_DisplayMode m;
	int display = 0;
	if(gSdlWin)
	{
		display = SDL_GetWindowDisplayIndex(gSdlWin);
		if(display < 0)
			display = 0;
	}
	if(Sdl_EnsureVideo() && SDL_GetDesktopDisplayMode(display, &m) == 0 && m.w > 0 && m.h > 0)
	{
		gSdlScreenW = m.w;
		gSdlScreenH = m.h;
	}
	*w = gSdlScreenW;
	*h = gSdlScreenH;
}

// the frame's total width and height: what the window manager put around the client area
void OS_FrameMetrics(int* fw, int* fh)
{
	int top = 0, left = 0, bottom = 0, right = 0;
	if(gSdlWin)
		SDL_GetWindowBordersSize(gSdlWin, &top, &left, &bottom, &right);
	*fw = left + right;
	*fh = top + bottom;
}

int OS_FrameTop(void)
{
	int top = 0, left = 0, bottom = 0, right = 0;
	if(gSdlWin)
		SDL_GetWindowBordersSize(gSdlWin, &top, &left, &bottom, &right);
	return top;
}

// ---- the cursor ---------------------------------------------------------------------------------

static int gCursorShape = OS_CURSOR_ARROW; // OS_CursorSetShape
static int gCursorCount;                   // ShowCursor: visible while >= 0
static int gCursorShown = -1;              // what SDL was told last, -1 = not yet
static SDL_Cursor* gCursorArrow;
static SDL_Cursor* gCursorCustom;

/* the cursor from the shape and the show count: hidden while the count is
 * negative or the shape is OS_CURSOR_NONE, else the arrow or a hand standing
 * in for the original's resource cursor 0x6A */
void Sdl_CursorApply(void)
{
	SDL_Cursor* c;
	int show;
	if(!gSdlWin)
		return;
	show = gCursorCount >= 0 && gCursorShape != OS_CURSOR_NONE;
	if(show && !gCursorArrow)
		gCursorArrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
	if(show && !gCursorCustom)
		gCursorCustom = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
	if(gCursorShown != show)
	{
		SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
		gCursorShown = show;
	}
	if(!show)
		return;
	c = gCursorShape == OS_CURSOR_CUSTOM && gCursorCustom ? gCursorCustom : gCursorArrow;
	if(c)
		SDL_SetCursor(c);
}

void OS_CursorSetShape(int shape)
{
	gCursorShape = shape;
	Sdl_CursorApply();
}

// ShowCursor's counter: +1 / -1, the cursor is visible while the count is not negative
void OS_CursorShow(int show)
{
	gCursorCount += show ? 1 : -1;
	Sdl_CursorApply();
}

// the client area's top-left on the screen
static void ClientOrigin(int* x, int* y)
{
	*x = *y = 0;
	if(gSdlWin)
		SDL_GetWindowPosition(gSdlWin, x, y);
}

// 1 when the client area and the picture are of different sizes (or full screen): the pointer must be mapped
static int PointerMapped(void)
{
	return gSdlFullscreen || gSdlPicW != gSdlClientW || gSdlPicH != gSdlClientH;
}

/* The pointer in screen coordinates (in full screen: picture coordinates,
 * since the engine's "screen" is the picture there).  In a windowed client
 * area of another size the client part is in picture pixels, so that
 * OS_ScreenToClient's subtraction of the client origin lands there too. */
void OS_CursorGetPos(int* x, int* y)
{
	int gx = 0, gy = 0, ox, oy;
#if defined(__ANDROID__)
	int tx, ty;
#endif
	*x = *y = 0;
	if(!gSdlWin)
		return;
#if defined(__ANDROID__)
	/* there is no mouse and SDL's own touch-to-mouse synthesis is off, so
	 * its global mouse state never moves: the pointer is the finger, in
	 * picture pixels, which is what this function reports in full screen
	 * (the engine's screen is the picture there).  Once the finger is up
	 * and its click has gone out (Sdl_TouchPointer), there is no pointer
	 * any more and the call falls through to the same state the engine had
	 * before the first touch: SDL's mouse, (0, 0) */
	if(Sdl_TouchPointer(&tx, &ty))
	{
		*x = tx;
		*y = ty;
		return;
	}
#endif
	SDL_GetGlobalMouseState(&gx, &gy);
	ClientOrigin(&ox, &oy);
	if(PointerMapped())
	{
		int px, py;
		Sdl_WindowToPicture((float)(gx - ox), (float)(gy - oy), &px, &py);
		if(gSdlFullscreen)
		{
			*x = px;
			*y = py;
		}
		else
		{
			*x = ox + px;
			*y = oy + py;
		}
	}
	else
	{
		*x = gx;
		*y = gy;
	}
}

// SetCursorPos: warp the pointer; the coordinates go through the same mapping as OS_CursorGetPos
void OS_CursorSetPos(int x, int y)
{
	int ox, oy, wx = 0, wy = 0;
	if(!gSdlWin)
		return;
	ClientOrigin(&ox, &oy);
	if(PointerMapped())
	{
		if(gSdlFullscreen)
			Sdl_PictureToWindow((float)x, (float)y, &wx, &wy);
		else
			Sdl_PictureToWindow((float)(x - ox), (float)(y - oy), &wx, &wy);
		SDL_WarpMouseGlobal(ox + wx, oy + wy);
	}
	else
		SDL_WarpMouseGlobal(x, y);
}

// screen -> client coordinates; in full screen the engine's "screen" is the picture, which is the client area
void OS_ScreenToClient(int* x, int* y)
{
	int ox, oy;
	if(gSdlFullscreen)
		return;
	ClientOrigin(&ox, &oy);
	*x -= ox;
	*y -= oy;
}

// client -> screen coordinates (the identity in full screen, as above)
void OS_ClientToScreen(int* x, int* y)
{
	int ox, oy;
	if(gSdlFullscreen)
		return;
	ClientOrigin(&ox, &oy);
	*x += ox;
	*y += oy;
}

// SDL does not report the desktop's button mapping: assume a right-handed mouse, as a fresh install
int OS_MouseButtonsSwapped(void)
{
	return 0;
}

// ---- key state ----------------------------------------------------------------------------------

// record a key or mouse button for OS_KeyState
void Sdl_NoteKey(int vk, int down)
{
	if(vk > 0 && vk < 256)
		gSdlKeys[vk] = (uint8_t)(down ? 1 : 0);
}

// GetAsyncKeyState & 0x8000: the key is down, from the presses and releases the pump has seen
int OS_KeyState(int vk)
{
	if(vk < 0 || vk > 255)
		return 0;
	return gSdlKeys[vk];
}

// GetKeyboardState: bit 7 of each entry from the same record; the toggle bits (Caps Lock etc.) are not tracked
void OS_KeyboardState(uint8_t out[256])
{
	int i;
	for(i = 0; i < 256; i++)
		out[i] = gSdlKeys[i] ? 0x80 : 0;
}
