/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * events.c - the event pump of the SDL back end: SDL events turned into the
 *            engine's OsEventHandlers_t callbacks, and the clipboard
 *            (inc/bgi/os.h)
 *
 * OS_PumpMessages takes one event per call, as the POSIX back end takes one
 * X event, and routes it: keys become key_down / syskey / key_up (Alt+key,
 * F10 and Alt itself are the "system" keys Windows delivers separately),
 * the mouse becomes mouse_move / mouse_button / double_click / mouse_wheel
 * with the position mapped into picture pixels, and the window events become
 * activate, sized, paint, close_request and display_change.  The key and
 * button state OS_KeyState reports is kept here through Sdl_NoteKey (sys.c).
 *
 * SDL's scancodes are used rather than its key symbols, so that a press is
 * the same virtual key whatever the layout's punctuation does (Shift+1 is
 * still VK '1', as on Windows); the punctuation keys get the VK_OEM_* codes
 * of a US layout, and the Japanese keys the codes of a JIS keyboard.
 */
#include "sdl_internal.h"

#include <string.h>

OsEventHandlers_t gSdlHandlers; // the engine's callbacks; every pointer is checked before the call

static int gDragAccept; // OS_DragAccept: whether `drop_file` is raised

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	gSdlHandlers = *h;
}

void OS_PostQuit(void)
{
	gSdlQuitPosted = 1; // the next pump raises `quit`
}

// drag and drop of files onto the window (SDL_DROPFILE)
void OS_DragAccept(int accept)
{
	gDragAccept = accept;
}

// SetClipboardData(CF_TEXT): the text as UTF-8, which is what SDL hands other programs
int OS_ClipboardSetText(const char* sjis)
{
	char utf8[0x2000];
	OsPosix_SjisToUtf8(sjis ? sjis : "", utf8, sizeof utf8);
	return SDL_SetClipboardText(utf8) == 0;
}

// ---- touch (Android) ----------------------------------------------------------------------------

#if defined(__ANDROID__)

/* Touch input.  One finger is the pointer: a tap is the left button, a long
 * press or a two-finger tap the right one, and a mostly vertical swipe the
 * wheel.  SDL's touch-to-mouse synthesis is off: it presses the left button
 * as soon as a finger goes down, before a tap can be told from a long press
 * or a two-finger tap.
 *
 * The engine polls the pointer once per frame, so a click is sent in two
 * parts: the press TOUCH_PRESS_MS after the finger went down, the release
 * TOUCH_HOLD_MS after the press.  OS_PumpMessages sends what is due and the
 * polled state follows the same calls.
 *
 * Back opens the menu, which is the right-button action in src/sys/window.c. */

#define TOUCH_TAP_MS     500  // a press held this long without movement is a long press
#define TOUCH_MOVE_PX    12   // movement beyond this is no longer a tap (picture pixels)
#define TOUCH_SWIPE_PX   48   // vertical movement per wheel notch (picture pixels)
#define TOUCH_SWIPE_AXIS 3    // |dy| * 2 > |dx| * TOUCH_SWIPE_AXIS makes a swipe vertical
#define TOUCH_PRESS_MS   40   // the pointer rests this long (about two frames) before a press
#define TOUCH_HOLD_MS    80   // and the button stays down at least this long

static int gTouchFingers;                 // fingers down
static int gTouchPrimary;                 // the first finger's id (-1 when none)
static uint32_t gTouchStart;              // when it went down
static int gTouchStartX, gTouchStartY;    // where it went down, picture pixels
static int gTouchLastX, gTouchLastY;      // and where it is now
static int gTouchLeftDown;                // the left button is down (a drag)
static int gTouchMoved;                   // the finger left the tap area
static int gTouchTwo;                     // a second finger joined
static int gTouchUsed;                    // a gesture was sent (no tap on the release)
static int gTouchLongFired;               // the long press opened the right button
static int gTouchScroll;                  // vertical movement banked for the wheel
static int gTouchUi;                      // this gesture is the corner button, not the game
static int gSdlPointerX = -1, gSdlPointerY = -1; // the last pointer position, picture pixels

/* The in-game button row (Skip, Auto, Save, Load, Log, ...) is hover-driven,
 * not a menu: iconmngr._bp keeps its 1026 x 64 window at (217, 950) - a 10 px
 * sliver at the bottom edge - and slides it up to (217, 896) while the
 * pointer is inside that strip (sub_0864).  A mouse holds it up by resting
 * there; a finger cannot, because it has to lift to let go.  A small button
 * in the picture's top-right corner (drawn by the renderer, on top of the
 * picture, not into the engine's texture) pins the polled pointer to a point
 * of the strip, so the engine's own hover keeps the row up; tapping the
 * button again lets the pointer go and the row slides back down.  While a
 * finger is down the pointer is the finger again, so the row's buttons are
 * clicked where they are. */
static int gTouchBar; // the row is held up by the corner button

// a button press or release that waits for its time (see Touch_QueueClick)
enum { TOUCH_DOWN, TOUCH_UP };

typedef struct TouchAction
{
	int what;     // TOUCH_DOWN / TOUCH_UP
	int button;   // 0 left, 1 right
	int x, y;     // picture pixels
	uint32_t due; // SDL_GetTicks at which it may be sent
} TouchAction_t;

static TouchAction_t gTouchQueue[8];
static int gTouchQueueN;          // actions waiting
static int gTouchPressed = -1;    // the button this gesture has queued a press for (-1 none)
static int gTouchDownButton = -1; // the button that is down now (-1 none)
static uint32_t gTouchDownTime;   // when its press went out

static int TouchAbs(int v)
{
	return v < 0 ? -v : v;
}

// the first 50 button reports are logged
static int gTouchLoggedButtons;

static void Touch_Mouse(int idx, int down, int x, int y)
{
	Sdl_NoteKey(idx == 0 ? 1 : 2, down); // VK_LBUTTON / VK_RBUTTON: the polled state follows
	if(gTouchLoggedButtons < 50)
	{
		gTouchLoggedButtons++;
		fprintf(stderr, "sdl: android: button %s %s at %d,%d, t=%u ms\n",
			idx == 0 ? "left" : "right", down ? "down" : "up", x, y, (unsigned)SDL_GetTicks());
		fflush(stderr);
	}
	if(gSdlHandlers.mouse_button)
		gSdlHandlers.mouse_button(idx, down, x, y);
}

// send one queued press or release now (the engine's event handler and the polled state together)
static void Touch_SendAction(const TouchAction_t* a)
{
	uint32_t now = SDL_GetTicks();
	int i;
	if(a->what == TOUCH_DOWN)
	{
		gTouchDownButton = a->button;
		gTouchDownTime = now;
		// the release may not come before the hold is over, however late the press went out
		for(i = 0; i < gTouchQueueN; i++)
			if(gTouchQueue[i].what == TOUCH_UP && gTouchQueue[i].button == a->button &&
				(int32_t)(gTouchQueue[i].due - (now + TOUCH_HOLD_MS)) < 0)
				gTouchQueue[i].due = now + TOUCH_HOLD_MS;
	}
	else if(gTouchDownButton == a->button)
		gTouchDownButton = -1;
	/* the event position and the polled pointer, for the log */
	fprintf(stderr,
		"sdl: android: send %s %s at %d,%d; polled pointer %d,%d, finger %d, bar %d\n",
		a->button == 0 ? "left" : "right", a->what == TOUCH_DOWN ? "down" : "up",
		a->x, a->y, gSdlPointerX, gSdlPointerY, gTouchFingers, gTouchBar);
	fflush(stderr);
	Touch_Mouse(a->button, a->what == TOUCH_DOWN, a->x, a->y);
}

// send every action whose time has come, in the order they were queued
static void Touch_Flush(void)
{
	while(gTouchQueueN > 0 && (int32_t)(SDL_GetTicks() - gTouchQueue[0].due) >= 0)
	{
		TouchAction_t a = gTouchQueue[0];
		gTouchQueueN--;
		memmove(&gTouchQueue[0], &gTouchQueue[1], (size_t)gTouchQueueN * sizeof gTouchQueue[0]);
		Touch_SendAction(&a);
	}
}

static void Touch_Enqueue(int what, int button, int x, int y, uint32_t due)
{
	TouchAction_t* a;
	if(gTouchQueueN == (int)(sizeof gTouchQueue / sizeof gTouchQueue[0]))
	{ // the pump was away for a while: send the oldest rather than lose it
		TouchAction_t first = gTouchQueue[0];
		gTouchQueueN--;
		memmove(&gTouchQueue[0], &gTouchQueue[1], (size_t)gTouchQueueN * sizeof gTouchQueue[0]);
		Touch_SendAction(&first);
	}
	a = &gTouchQueue[gTouchQueueN++];
	a->what = what;
	a->button = button;
	a->x = x;
	a->y = y;
	a->due = due;
}

/* a complete click: the press at `downDue`, the release TOUCH_HOLD_MS after
 * it; only the release is added when the press is queued or down already */
static void Touch_QueueClick(int button, int x, int y, uint32_t downDue)
{
	if(gTouchPressed == button)
	{
		Touch_Enqueue(TOUCH_UP, button, x, y,
			gTouchDownButton == button ? gTouchDownTime + TOUCH_HOLD_MS : 0);
		return;
	}
	gTouchPressed = button;
	Touch_Enqueue(TOUCH_DOWN, button, x, y, downDue);
	Touch_Enqueue(TOUCH_UP, button, x, y, 0); // 0: the hold is counted from the press
}

// a complete right click at (x, y): the press now, the release after the minimum hold
static void Touch_RightClick(int x, int y)
{
	Touch_QueueClick(1, x, y, SDL_GetTicks());
}

// the end of a drag: the left button goes up, not before the minimum hold
static void Touch_ReleaseDrag(void)
{
	if(!gTouchLeftDown)
		return;
	gTouchLeftDown = 0;
	Touch_Enqueue(TOUCH_UP, 0, gTouchLastX, gTouchLastY,
		gTouchDownButton == 0 ? gTouchDownTime + TOUCH_HOLD_MS : 0);
}

/* the row's hover strip: (222, 928) of the 1280 x 960 screen, inside the row
 * but left of its first button */
static void Touch_RowPoint(int* x, int* y)
{
	*x = gSdlPicW * 222 / 1280;
	*y = gSdlPicH * 928 / 960;
	if(*x >= gSdlPicW)
		*x = gSdlPicW - 1;
	if(*y >= gSdlPicH)
		*y = gSdlPicH - 1;
}

// the corner button: a square at the picture's top-right, a twelfth of its height
static void Touch_BarRect(int* l, int* t, int* r, int* b)
{
	int size = gSdlPicH / 12; // about 80 px of a 1280 x 960 picture
	int margin = size / 3;
	if(size < 32)
		size = 32;
	if(size > gSdlPicW / 4)
		size = gSdlPicW / 4; // never more than a quarter of the picture's width
	*l = gSdlPicW - size - margin;
	*t = margin;
	if(*l < 0)
		*l = 0;
	if(*t < 0)
		*t = 0;
	*r = *l + size;
	*b = *t + size;
}

// a little larger than the square is drawn, so that a tap need not be exact
static int Touch_BarHit(int x, int y)
{
	int l, t, r, b, slop;
	Touch_BarRect(&l, &t, &r, &b);
	slop = (r - l) / 4;
	return x >= l - slop && x < r + slop && y >= t - slop && y < b + slop;
}

// a press still waiting for the frame that may send it?
static int Touch_PressWaiting(void)
{
	int i;
	for(i = 0; i < gTouchQueueN; i++)
		if(gTouchQueue[i].what == TOUCH_DOWN)
			return 1;
	return 0;
}

// log the polled pointer when it changes, not every frame
static void Touch_LogPoll(int have, int x, int y)
{
	static const char* const names[4] = {"none", "finger", "finger (stored)", "pin"};
	static int lastHave = -1, lastX = -1, lastY = -1, lastSrc = -1;
	int src = !have ? 0 : gTouchFingers > 0 ? 1 : gTouchBar ? 3 : 2;
	if(src == 1 && !Touch_PressWaiting())
		return; // a drag or a held finger is not a press being placed
	if(src == lastSrc && have == lastHave && (!have || (x == lastX && y == lastY)))
		return;
	lastHave = have;
	lastX = x;
	lastY = y;
	lastSrc = src;
	fprintf(stderr,
		"sdl: android: pointer poll %d,%d (%s), finger %d, queue %d, bar %d\n",
		x, y, names[src], gTouchFingers, gTouchQueueN, gTouchBar);
	fflush(stderr);
}

// the pointer OS_CursorGetPos reports: the finger while it is down or a click
// waits, else the corner button's row point while the row is pinned; 0 when
// there is no pointer
int Sdl_TouchPointer(int* x, int* y)
{
	int have = 0, px = 0, py = 0;
	if(gTouchFingers > 0 || gTouchQueueN > 0)
	{
		if(gSdlPointerX >= 0 && gSdlPointerY >= 0)
		{
			px = gSdlPointerX;
			py = gSdlPointerY;
			have = 1;
		}
	}
	else
	{
		gSdlPointerX = -1; // no finger: the pointer is gone
		gSdlPointerY = -1;
		if(gTouchBar)
		{
			Touch_RowPoint(&px, &py);
			have = 1;
		}
	}
	Touch_LogPoll(have, px, py);
	if(have)
	{
		*x = px;
		*y = py;
	}
	return have;
}

/* the corner button over the picture: a dark square with three bars, brighter
 * while the row is up */
void Sdl_DrawTouchBar(SDL_Renderer* ren)
{
	int l, t, r, b, w, i;
	SDL_Rect box, bar;
	if(!ren)
		return;
	Touch_BarRect(&l, &t, &r, &b);
	w = r - l;
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(ren, 0, 0, 0, gTouchBar ? 176 : 104);
	box.x = l;
	box.y = t;
	box.w = w;
	box.h = b - t;
	SDL_RenderFillRect(ren, &box);
	if(gTouchBar)
	{
		SDL_SetRenderDrawColor(ren, 255, 255, 255, 196);
		SDL_RenderDrawRect(ren, &box);
	}
	SDL_SetRenderDrawColor(ren, 255, 255, 255, 220);
	bar.w = w * 3 / 5;
	bar.x = l + (w - bar.w) / 2;
	bar.h = w / 10 > 2 ? w / 10 : 2;
	for(i = 0; i < 3; i++)
	{
		bar.y = t + (b - t) / 2 - bar.h * 2 + i * bar.h * 2;
		SDL_RenderFillRect(ren, &bar);
	}
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

// the pointer position for the Back button before any touch: the picture's centre
static int TouchPointerX(void)
{
	return gSdlPointerX >= 0 ? gSdlPointerX : gSdlPicW / 2;
}

static int TouchPointerY(void)
{
	return gSdlPointerY >= 0 ? gSdlPointerY : gSdlPicH / 2;
}

// the window in points: what SDL_RenderWindowToLogical takes (not the renderer's output pixels)
static void Touch_WindowSize(int* w, int* h)
{
	if(gSdlWin)
		SDL_GetWindowSize(gSdlWin, w, h);
	else
	{
		*w = gSdlClientW;
		*h = gSdlClientH;
	}
	if(*w <= 0)
		*w = 1;
	if(*h <= 0)
		*h = 1;
}

/* A finger position in picture pixels.  SDL renormalises finger events into
 * the renderer's viewport, so with a logical size set the position is already
 * 0..1 over the picture; without one it goes through the window mapping.
 * Returns 1 for the direct path. */
static int Touch_Picture(float nx, float ny, int* x, int* y)
{
	int lw = 0, lh = 0, w, h;
	if(gSdlRen)
		SDL_RenderGetLogicalSize(gSdlRen, &lw, &lh);
	if(lw > 0 && lh > 0)
	{
		*x = (int)(nx * (float)gSdlPicW);
		*y = (int)(ny * (float)gSdlPicH);
		if(*x < 0)
			*x = 0;
		else if(*x >= gSdlPicW)
			*x = gSdlPicW - 1;
		if(*y < 0)
			*y = 0;
		else if(*y >= gSdlPicH)
			*y = gSdlPicH - 1;
		return 1;
	}
	Touch_WindowSize(&w, &h);
	Sdl_WindowToPicture(nx * (float)w, ny * (float)h, x, y);
	return 0;
}

/* the first 50 finger events, and every tap release, with the raw, window and
 * picture positions */
static int gTouchLogged;

static void Touch_Log(const SDL_TouchFingerEvent* t, const char* what)
{
	int w, h, wx, wy, x, y;
	int tap = what[0] == 'u';
	if(gTouchLogged >= 50 && !tap)
		return;
	gTouchLogged++;
	Touch_WindowSize(&w, &h);
	wx = (int)(t->x * (float)w);
	wy = (int)(t->y * (float)h);
	Touch_Picture(t->x, t->y, &x, &y); // the same mapping the event itself took
	fprintf(stderr,
		"sdl: android: finger %s id=%d raw %.4f,%.4f -> window %d,%d -> picture %d,%d "
		"(window %dx%d pt, picture %dx%d)\n",
		what, (int)t->fingerId, t->x, t->y, wx, wy, x, y, w, h, gSdlPicW, gSdlPicH);
	fflush(stderr);
}

/* One line at the touch-down with every input of the finger-to-picture
 * mapping: raw position, path, window / output / logical sizes, viewport,
 * scale, the computed picture pixel and the direct alternative. */
static void Touch_LogDown(const SDL_TouchFingerEvent* t, int px, int py, int direct)
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
		"sdl: android: touch down id=%d raw %.4f,%.4f path %s window %dx%d output %dx%d "
		"logical %dx%d viewport %d,%d %dx%d scale %.4f,%.4f picture %dx%d client %dx%d "
		"-> window %.1f,%.1f -> picture %d,%d alt %.0f,%.0f (sdl %d.%d.%d)\n",
		(int)t->fingerId, t->x, t->y, direct ? "logical" : "window",
		ww, wh, ow, oh, lw, lh,
		vp.x, vp.y, vp.w, vp.h, sx, sy, gSdlPicW, gSdlPicH, gSdlClientW, gSdlClientH,
		t->x * (float)ww, t->y * (float)wh, px, py,
		t->x * (float)gSdlPicW, t->y * (float)gSdlPicH, v.major, v.minor, v.patch);
	fflush(stderr);
}

static void Touch_Down(SDL_TouchFingerEvent* t)
{
	if(gTouchFingers == 0)
	{ // a new gesture: the finger that goes down is its pointer, the others only its second
		gTouchPrimary = (int)t->fingerId;
		gTouchStart = SDL_GetTicks();
		{
			int direct = Touch_Picture(t->x, t->y, &gTouchStartX, &gTouchStartY);
			Touch_LogDown(t, gTouchStartX, gTouchStartY, direct);
		}
		gTouchLastX = gTouchStartX;
		gTouchLastY = gTouchStartY;
		gTouchLeftDown = gTouchMoved = gTouchTwo = gTouchUsed = gTouchLongFired = gTouchUi = 0;
		gTouchScroll = 0;
		if(Touch_BarHit(gTouchStartX, gTouchStartY))
		{ /* the corner button: the whole gesture belongs to the app, no
		   * pointer, move, press or wheel of it may reach the game (a click
		   * of the last gesture that is due still goes out first) */
			gTouchUi = 1;
			Touch_Flush();
		}
		else if(gTouchPressed == -1 && gTouchDownButton == -1)
			gTouchQueueN = 0; // an action still waiting of the previous gesture is over with it
		gTouchPressed = -1;
		if(!gTouchUi)
		{
			gSdlPointerX = gTouchStartX; // the polled pointer: OS_CursorGetPos reports this
			gSdlPointerY = gTouchStartY;
			if(gSdlHandlers.mouse_move) // and the move event, as a mouse would send before the press
				gSdlHandlers.mouse_move(gTouchStartX, gTouchStartY);
		}
	}
	else if(gTouchFingers == 1)
	{ // a second finger: no left click may be in the way of the two-finger tap
		gTouchTwo = 1;
		Touch_ReleaseDrag();
	}
	if(gTouchFingers < 2)
		gTouchFingers++;
}

static void Touch_Up(SDL_TouchFingerEvent* t)
{
	uint32_t held, now = SDL_GetTicks(), due;
	if(gTouchFingers > 0)
		gTouchFingers--;
	if((int)t->fingerId != gTouchPrimary)
		return; // the second finger; the tap fires when the first one goes
	held = now - gTouchStart;
	if(gTouchUi)
	{ // the corner button: a tap (no drag, no second finger) shows or hides the row
		if(!gTouchMoved && !gTouchTwo)
			gTouchBar = !gTouchBar;
	}
	else if(gTouchTwo)
	{
		if(!gTouchMoved && !gTouchUsed && held < TOUCH_TAP_MS)
			Touch_RightClick(gTouchLastX, gTouchLastY);
	}
	else if(gTouchLeftDown)
		Touch_ReleaseDrag(); // the end of a drag
	else if(gTouchLongFired)
		; // the long press already opened the menu
	else if(!gTouchMoved && !gTouchUsed && held < TOUCH_TAP_MS)
	{ /* a tap: one left click.  The press waits until the pointer has been
	   * on the spot for TOUCH_PRESS_MS, so a frame has found the item under
	   * the cursor before the button goes down */
		due = gTouchStart + TOUCH_PRESS_MS;
		if((int32_t)(due - now) < 0)
			due = now;
		Touch_QueueClick(0, gTouchStartX, gTouchStartY, due);
	}
	else if(!gTouchMoved && !gTouchUsed)
		Touch_RightClick(gTouchLastX, gTouchLastY); // held long, the timer was late
	gTouchPrimary = -1;
	gTouchPressed = -1;
	gTouchTwo = gTouchMoved = gTouchUsed = gTouchUi = 0;
}

static void Touch_Motion(SDL_TouchFingerEvent* t)
{
	int x, y, dx, dy;
	if((int)t->fingerId != gTouchPrimary)
		return;
	Touch_Picture(t->x, t->y, &x, &y);
	dx = x - gTouchStartX;
	dy = y - gTouchStartY;
	if(gTouchUi)
	{ // the corner button: nothing of this gesture reaches the game
		if(!gTouchMoved && (TouchAbs(dx) > TOUCH_MOVE_PX || TouchAbs(dy) > TOUCH_MOVE_PX))
			gTouchMoved = 1;
		gTouchLastX = x;
		gTouchLastY = y;
		return;
	}
	gSdlPointerX = x;
	gSdlPointerY = y;
	if(!gTouchMoved && (TouchAbs(dx) > TOUCH_MOVE_PX || TouchAbs(dy) > TOUCH_MOVE_PX))
		gTouchMoved = 1;
	if(!gTouchMoved || gTouchTwo)
	{
		gTouchLastX = x;
		gTouchLastY = y;
		return;
	}
	if(!gTouchLeftDown && TouchAbs(dy) * 2 > TouchAbs(dx) * TOUCH_SWIPE_AXIS)
	{ // a vertical swipe: the wheel, a notch every TOUCH_SWIPE_PX
		gTouchUsed = 1;
		gTouchScroll += y - gTouchLastY; // positive (down) away from the user
		while(gTouchScroll >= TOUCH_SWIPE_PX)
		{
			if(gSdlHandlers.mouse_wheel)
				gSdlHandlers.mouse_wheel(120);
			gTouchScroll -= TOUCH_SWIPE_PX;
		}
		while(gTouchScroll <= -TOUCH_SWIPE_PX)
		{
			if(gSdlHandlers.mouse_wheel)
				gSdlHandlers.mouse_wheel(-120);
			gTouchScroll += TOUCH_SWIPE_PX;
		}
	}
	else
	{ // a drag: the left button goes down where the finger went
		if(!gTouchLeftDown)
		{
			gTouchLeftDown = 1;
			gTouchPressed = 0;
			Touch_Enqueue(TOUCH_DOWN, 0, gTouchStartX, gTouchStartY, SDL_GetTicks());
		}
		Touch_Flush(); // the press goes out before the moves that follow it
		if(gTouchDownButton == 0 && gSdlHandlers.mouse_move)
			gSdlHandlers.mouse_move(x, y);
	}
	gTouchLastX = x;
	gTouchLastY = y;
}

// a long press fires while the finger is still down, not on the release
static void Touch_Poll(void)
{
	if(!gTouchFingers || gTouchUi || gTouchTwo || gTouchMoved || gTouchLeftDown || gTouchLongFired)
		return;
	if(SDL_GetTicks() - gTouchStart < TOUCH_TAP_MS)
		return;
	gTouchLongFired = 1;
	gTouchUsed = 1;
	Touch_RightClick(gTouchLastX, gTouchLastY);
}

// once: the hints the touch layer needs, and no screen blanking while reading
static void Android_Init(void)
{
	static int done;
	if(done)
		return;
	done = 1;
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); // this file synthesises the mouse
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1"); // Back reaches the app
	SDL_DisableScreenSaver();
}

#endif // __ANDROID__

// ---- keys ---------------------------------------------------------------------------------------

/* SDL scancode -> Windows virtual key (0 = none).  Letters and digits map to
 * their ASCII codes, the function keys to VK_F1 .., the keypad to
 * VK_NUMPAD0 ..; the punctuation keys get the VK_OEM_* codes of a US layout,
 * so a script that tests them sees what it would on Windows. */
static int ScancodeToVk(SDL_Scancode sc)
{
	if(sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
		return 'A' + (int)(sc - SDL_SCANCODE_A);
	if(sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
		return '1' + (int)(sc - SDL_SCANCODE_1);
	if(sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
		return 0x70 + (int)(sc - SDL_SCANCODE_F1);
	if(sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24)
		return 0x7c + (int)(sc - SDL_SCANCODE_F13);
	if(sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
		return 0x61 + (int)(sc - SDL_SCANCODE_KP_1);
	switch(sc)
	{
		case SDL_SCANCODE_0: return '0';
		case SDL_SCANCODE_KP_0: return 0x60;
		case SDL_SCANCODE_ESCAPE: return 0x1b;
		case SDL_SCANCODE_BACKSPACE: return 0x08;
		case SDL_SCANCODE_TAB: return 0x09;
		case SDL_SCANCODE_CLEAR: return 0x0c;
		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_KP_ENTER: return 0x0d;
		case SDL_SCANCODE_LSHIFT:
		case SDL_SCANCODE_RSHIFT: return 0x10;
		case SDL_SCANCODE_LCTRL:
		case SDL_SCANCODE_RCTRL: return 0x11;
		case SDL_SCANCODE_LALT:
		case SDL_SCANCODE_RALT: return 0x12;
		case SDL_SCANCODE_LGUI: return 0x5b;
		case SDL_SCANCODE_RGUI: return 0x5c;
		case SDL_SCANCODE_PAUSE: return 0x13;
		case SDL_SCANCODE_CAPSLOCK: return 0x14;
		case SDL_SCANCODE_SPACE: return 0x20;
		case SDL_SCANCODE_PAGEUP: return 0x21;
		case SDL_SCANCODE_PAGEDOWN: return 0x22;
		case SDL_SCANCODE_END: return 0x23;
		case SDL_SCANCODE_HOME: return 0x24;
		case SDL_SCANCODE_LEFT: return 0x25;
		case SDL_SCANCODE_UP: return 0x26;
		case SDL_SCANCODE_RIGHT: return 0x27;
		case SDL_SCANCODE_DOWN: return 0x28;
		case SDL_SCANCODE_PRINTSCREEN: return 0x2c;
		case SDL_SCANCODE_INSERT: return 0x2d;
		case SDL_SCANCODE_DELETE: return 0x2e;
		case SDL_SCANCODE_HELP: return 0x2f;
		case SDL_SCANCODE_APPLICATION: return 0x5d;
		case SDL_SCANCODE_KP_MULTIPLY: return 0x6a;
		case SDL_SCANCODE_KP_PLUS: return 0x6b;
		case SDL_SCANCODE_KP_COMMA: return 0x6c;
		case SDL_SCANCODE_KP_MINUS: return 0x6d;
		case SDL_SCANCODE_KP_PERIOD: return 0x6e;
		case SDL_SCANCODE_KP_DIVIDE: return 0x6f;
		case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
		case SDL_SCANCODE_SCROLLLOCK: return 0x91;
		case SDL_SCANCODE_SEMICOLON: return 0xba;
		case SDL_SCANCODE_EQUALS: return 0xbb;
		case SDL_SCANCODE_COMMA: return 0xbc;
		case SDL_SCANCODE_MINUS: return 0xbd;
		case SDL_SCANCODE_PERIOD: return 0xbe;
		case SDL_SCANCODE_SLASH: return 0xbf;
		case SDL_SCANCODE_GRAVE: return 0xc0;
		case SDL_SCANCODE_LEFTBRACKET: return 0xdb;
		case SDL_SCANCODE_BACKSLASH:
		case SDL_SCANCODE_INTERNATIONAL1: return 0xdc;
		case SDL_SCANCODE_RIGHTBRACKET: return 0xdd;
		case SDL_SCANCODE_APOSTROPHE: return 0xde;
		// the Japanese keys: Kanji, Convert, NonConvert, the kana and the zenkaku key
		case SDL_SCANCODE_INTERNATIONAL2: return 0x19; // VK_KANJI
		case SDL_SCANCODE_INTERNATIONAL4: return 0x1c; // VK_CONVERT
		case SDL_SCANCODE_INTERNATIONAL5: return 0x1d; // VK_NONCONVERT
		case SDL_SCANCODE_LANG1: return 0xf2;          // VK_OEM_COPY on Japanese keyboards
		case SDL_SCANCODE_INTERNATIONAL3: return 0xf3; // VK_OEM_AUTO
		default: return 0;
	}
}

/* a key press or release of the main window: key_down (with the auto-repeat
 * flag) / syskey / key_up as Windows would deliver them.  F10, Alt and
 * Alt+key are system keys (WM_SYSKEYDOWN); of their releases the original's
 * window procedure only looks at F10. */
static void HandleKey(SDL_KeyboardEvent* k, int down)
{
	int vk = ScancodeToVk(k->keysym.scancode);
	int sys;
#if defined(__ANDROID__)
	if(k->keysym.scancode == SDL_SCANCODE_AC_BACK)
	{
		/* the engine's menu is opened by a right press and by nothing
		 * else, so Back stands in for one */
		if(down && !k->repeat)
			Touch_RightClick(TouchPointerX(), TouchPointerY());
		return;
	}
#endif
	if(!vk)
		return;
	sys = vk == 0x12 || vk == 0x79 || (k->keysym.mod & KMOD_ALT) != 0;
	Sdl_NoteKey(vk, down);
	if(down)
	{
		if(sys)
		{
			if(gSdlHandlers.syskey)
				gSdlHandlers.syskey(vk);
		}
		else if(gSdlHandlers.key_down)
			gSdlHandlers.key_down(vk, k->repeat ? 1 : 0);
	}
	else if(sys)
	{
		if(vk == 0x79 && gSdlHandlers.key_up)
			gSdlHandlers.key_up(vk); // WM_SYSKEYUP: the original's window procedure only looks at F10
	}
	else if(gSdlHandlers.key_up)
		gSdlHandlers.key_up(vk);
}

// ---- mouse --------------------------------------------------------------------------------------

static const int kButtonVk[5] = {1, 2, 4, 5, 6}; // the virtual key of each button index (VK_LBUTTON ..)

// SDL button -> the engine's index (0 L 1 R 2 M 3 X1 4 X2), -1 for others
static int ButtonIndex(Uint8 button)
{
	switch(button)
	{
		case SDL_BUTTON_LEFT: return 0;
		case SDL_BUTTON_RIGHT: return 1;
		case SDL_BUTTON_MIDDLE: return 2;
		case SDL_BUTTON_X1: return 3;
		case SDL_BUTTON_X2: return 4;
		default: return -1;
	}
}

/* a mouse button press or release: the press records the button, detects a
 * double click (WM_?BUTTONDBLCLK, raised in front of the ordinary press, for
 * the left and right buttons only) and raises mouse_button in picture
 * pixels */
static void HandleButton(SDL_MouseButtonEvent* b, int down)
{
	int idx = ButtonIndex(b->button);
	int x, y;
	if(idx < 0)
		return;
	Sdl_WindowToPicture((float)b->x, (float)b->y, &x, &y);
	Sdl_NoteKey(kButtonVk[idx], down);
	if(down && idx <= 1 && b->clicks == 2 && gSdlHandlers.double_click)
		gSdlHandlers.double_click(idx);
	if(gSdlHandlers.mouse_button)
		gSdlHandlers.mouse_button(idx, down, x, y);
}

// the wheel: one notch is WHEEL_DELTA, positive away from the user
static void HandleWheel(SDL_MouseWheelEvent* w)
{
	int delta;
	if(!w->y)
		return;
	delta = (int)w->y * 120;
	if(w->direction == SDL_MOUSEWHEEL_FLIPPED)
		delta = -delta;
	if(gSdlHandlers.mouse_wheel)
		gSdlHandlers.mouse_wheel(delta);
}

// a pointer move, in picture pixels
static void HandleMotion(SDL_MouseMotionEvent* m)
{
	int x, y;
	Sdl_WindowToPicture((float)m->x, (float)m->y, &x, &y);
#if defined(__ANDROID__)
	gSdlPointerX = x; // where Back's synthetic right click lands
	gSdlPointerY = y;
#endif
	if(gSdlHandlers.mouse_move)
		gSdlHandlers.mouse_move(x, y);
}

#if defined(__ANDROID__)
// one finger went down / up / moved (a touch id, normalised coordinates)
static void HandleFinger(SDL_TouchFingerEvent* t, int what)
{
	Touch_Log(t, what == 0 ? "down" : what == 1 ? "up" : "move");
	switch(what)
	{
		case 0: Touch_Down(t); break;
		case 1: Touch_Up(t); break;
		default: Touch_Motion(t); break;
	}
}
#endif

// ---- window events ------------------------------------------------------------------------------

#if defined(__ANDROID__)
/* The activity can pause and resume in any order (permission dialog, home
 * button, rotation).  Any sign of being away pauses the audio and tells the
 * engine the window lost focus; any sign of being visible again resumes it
 * and activates the window. */
static void Android_Suspend(const char* why)
{
	if(gSdlActive)
	{
		gSdlActive = 0;
		if(gSdlHandlers.activate)
			gSdlHandlers.activate(0);
	}
	memset(gSdlKeys, 0, sizeof gSdlKeys); // nothing is held across a pause
	Sdl_AudioSuspend(1);
	fprintf(stderr, "sdl: android: paused (%s)\n", why);
}

static void Android_Resume(const char* why)
{
	int was = gSdlActive;
	if(gSdlMinimized)
	{
		gSdlMinimized = 0;
		if(gSdlHandlers.sized)
			gSdlHandlers.sized(0);
	}
	Sdl_AudioSuspend(0);
	gSdlActive = 1;
	if(!was && gSdlHandlers.activate)
		gSdlHandlers.activate(1);
	Sdl_SyncClientSize(); // the surface may have another size or density
	Sdl_ApplyLogical();   // and the logical size has to follow it
	if(gSdlHandlers.paint)
		gSdlHandlers.paint();
	fprintf(stderr, "sdl: android: resumed (%s)\n", why);
}
#endif

static void HandleWindowEvent(SDL_WindowEvent* w)
{
	switch(w->event)
	{
		case SDL_WINDOWEVENT_EXPOSED: // uncovered: repaint everything
			if(gSdlHandlers.paint)
				gSdlHandlers.paint();
			break;
		case SDL_WINDOWEVENT_SHOWN:
			gSdlShown = 1;
#if defined(__ANDROID__)
			Android_Resume("shown");
#endif
			break;
		case SDL_WINDOWEVENT_HIDDEN:
			gSdlShown = 0;
#if defined(__ANDROID__)
			Android_Suspend("hidden");
#endif
			break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			gSdlActive = 1;
			if(gSdlHandlers.activate)
				gSdlHandlers.activate(1);
#if defined(__ANDROID__)
			Android_Resume("focus gained");
#endif
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			gSdlActive = 0;
			memset(gSdlKeys, 0, sizeof gSdlKeys); // nothing is held across a focus loss
			if(gSdlHandlers.activate)
				gSdlHandlers.activate(0);
			break;
		case SDL_WINDOWEVENT_MINIMIZED:
			gSdlMinimized = 1;
			if(gSdlHandlers.sized)
				gSdlHandlers.sized(1);
#if defined(__ANDROID__)
			Android_Suspend("minimized");
#endif
			break;
		case SDL_WINDOWEVENT_RESTORED:
			if(gSdlMinimized)
			{
				gSdlMinimized = 0;
				if(gSdlHandlers.sized)
					gSdlHandlers.sized(0);
			}
#if defined(__ANDROID__)
			Android_Resume("restored");
#endif
			break;
		case SDL_WINDOWEVENT_SIZE_CHANGED: // the renderer redoes the scaling itself; the size is noted
			gSdlClientW = w->data1;
			gSdlClientH = w->data2;
#if defined(__ANDROID__)
			/* SDL re-applies the logical size on a resize but defers the
			 * viewport flush on Android: do it here, then repaint */
			Sdl_ApplyLogical();
			if(gSdlHandlers.paint)
				gSdlHandlers.paint();
#endif
			break;
		case SDL_WINDOWEVENT_DISPLAY_CHANGED: // another monitor or a resolution change
			OS_ScreenSize(&gSdlScreenW, &gSdlScreenH);
			if(gSdlHandlers.display_change)
				gSdlHandlers.display_change();
#if defined(__ANDROID__)
			Sdl_SyncClientSize();
			Sdl_ApplyLogical();
			if(gSdlHandlers.paint)
				gSdlHandlers.paint();
#endif
			break;
		default:
			break;
	}
}

// ---- the pump -----------------------------------------------------------------------------------

/* `quit` (OS_PostQuit, a signal) and the close request (OS_WindowClose) come
 * first - they are this back end's posted messages - then one SDL event;
 * 1 when something was delivered.  `quit` may not return (the engine throws
 * out of its loop). */
int OS_PumpMessages(void)
{
	SDL_Event ev;
#if defined(__ANDROID__)
	Android_Init();
	Touch_Poll();  // a long press fires while the finger is still down
	Touch_Flush(); // and the presses / releases whose time has come go out
#endif
	if(gSdlQuitPosted)
	{
		gSdlQuitPosted = 0;
		if(gSdlHandlers.quit)
			gSdlHandlers.quit();
		return 1;
	}
	if(gSdlCloseRequested)
	{
		gSdlCloseRequested = 0;
		if(gSdlHandlers.close_request)
			gSdlHandlers.close_request();
		return 1;
	}
	if(!gSdlWin)
		return 0;
	if(!SDL_PollEvent(&ev))
		return 0;
	switch(ev.type)
	{
		case SDL_QUIT: // the close box or Alt+F4
			if(gSdlHandlers.close_request)
				gSdlHandlers.close_request();
			break;
		case SDL_WINDOWEVENT:
			HandleWindowEvent(&ev.window);
			break;
		case SDL_KEYDOWN:
			HandleKey(&ev.key, 1);
			break;
		case SDL_KEYUP:
			HandleKey(&ev.key, 0);
			break;
		case SDL_MOUSEMOTION:
			HandleMotion(&ev.motion);
			break;
		case SDL_MOUSEBUTTONDOWN:
			HandleButton(&ev.button, 1);
			break;
		case SDL_MOUSEBUTTONUP:
			HandleButton(&ev.button, 0);
			break;
		case SDL_MOUSEWHEEL:
			HandleWheel(&ev.wheel);
			break;
#if defined(__ANDROID__)
		case SDL_FINGERDOWN:
			HandleFinger(&ev.tfinger, 0);
			break;
		case SDL_FINGERUP:
			HandleFinger(&ev.tfinger, 1);
			break;
		case SDL_FINGERMOTION:
			HandleFinger(&ev.tfinger, 2);
			break;
#endif
#if defined(__ANDROID__)
		case SDL_APP_WILLENTERBACKGROUND: // the activity goes away: no audio while it is gone
			Android_Suspend("app will enter background");
			break;
		case SDL_APP_DIDENTERFOREGROUND: // and back, with the surface ready
			Android_Resume("app did enter foreground");
			break;
#endif
		case SDL_DROPFILE: // a file dropped on the window (after OS_DragAccept(1))
			if(gDragAccept && gSdlHandlers.drop_file)
				gSdlHandlers.drop_file(ev.drop.file);
			SDL_free(ev.drop.file);
			break;
		default:
			break;
	}
	return 1;
}
