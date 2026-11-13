/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * unsupported.c - what the SDL back end does not have yet: the pop-up child
 *                 windows ("B0 1x"), the text entry control ("B0 2x") and
 *                 the debugger's own window (inc/bgi/os.h)
 *
 * Each function keeps the contract of a service that is not there, as
 * inc/bgi/os.h spells it out for a back end without a counterpart: a child
 * window cannot be created, the text entry stays empty, and the debugger is
 * told there is no window of its own (OS_DebugWindowOpen returns 0, so it
 * draws over the main window instead, which does work through the ordinary
 * blits).
 *
 * These are the next pieces to implement in Stage B; nothing here fakes a
 * window that the engine could then wait on.
 */
#include "sdl_internal.h"

// ---- child windows ("B0 1x", src/sys/child.c) --------------------------------------------------

// no child window: the engine frees the back buffer it took for it and draws it nowhere
OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	BGI_UNUSED(index);
	BGI_UNUSED(title);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(outerW);
	BGI_UNUSED(outerH);
	return NULL;
}

void OS_ChildClose(OsChild_t* c)
{
	BGI_UNUSED(c);
}

void OS_ChildShow(OsChild_t* c, int show)
{
	BGI_UNUSED(c);
	BGI_UNUSED(show);
}

void OS_ChildSetTitle(OsChild_t* c, const char* title)
{
	BGI_UNUSED(c);
	BGI_UNUSED(title);
}

void OS_ChildMove(OsChild_t* c, int x, int y, int outerW, int outerH)
{
	BGI_UNUSED(c);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(outerW);
	BGI_UNUSED(outerH);
}

void OS_ChildGetPos(OsChild_t* c, int* x, int* y)
{
	BGI_UNUSED(c);
	*x = *y = 0;
}

OsDc_t* OS_ChildGetDc(OsChild_t* c)
{
	BGI_UNUSED(c);
	return NULL;
}

void OS_ChildReleaseDc(OsChild_t* c, OsDc_t* dc)
{
	BGI_UNUSED(c);
	BGI_UNUSED(dc);
}

// ---- the text entry ("B0 2x", src/sys/edit.c) ----------------------------------------------------
// No edit control: the field is never shown, and the engine sees an empty one.

void OS_EditShow(int show)
{
	BGI_UNUSED(show);
}

void OS_EditMove(int x, int y, int w, int h)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
}

void OS_EditSetFont(const char* face, int height, int width)
{
	BGI_UNUSED(face);
	BGI_UNUSED(height);
	BGI_UNUSED(width);
}

void OS_EditLimitText(int maxChars)
{
	BGI_UNUSED(maxChars);
}

void OS_EditSetText(const char* t)
{
	BGI_UNUSED(t);
}

void OS_EditSelectAll(void)
{
}

void OS_EditFocus(int toEdit)
{
	BGI_UNUSED(toEdit);
}

// the text in the control: there is none
int OS_EditGetText(char* buf, size_t n)
{
	if(n > 0)
		buf[0] = 0;
	return 0;
}

void OS_EditSetColour(uint32_t rgb)
{
	BGI_UNUSED(rgb);
}

// InvalidateRect + UpdateWindow of the control: there is nothing to repaint
void OS_EditRefresh(void)
{
}

void OS_EditDestroy(void)
{
}

void OS_EditRejectAscii(int reject)
{
	BGI_UNUSED(reject);
}

// the IME is never open on a control that does not exist
int OS_EditImeOpen(void)
{
	return 0;
}

// ---- the debugger's window (src/dbg/) ------------------------------------------------------------
// 0: no window of its own, so the debugger draws over the main window (which
// the OS layer does present), as inc/bgi/os.h allows.

int OS_DebugWindowOpen(int w, int h, const char* title)
{
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(title);
	return 0;
}

void OS_DebugWindowClose(void)
{
}

int OS_DebugWindowAlive(void)
{
	return 0;
}

void OS_DebugWindowPresent(const OsSurface_t* s)
{
	BGI_UNUSED(s);
}
