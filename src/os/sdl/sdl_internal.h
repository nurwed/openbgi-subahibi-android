/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sdl_internal.h - the shared state of the SDL back end (inc/bgi/os.h)
 *
 * The back end is assembled from two sets of files: the X11-free files of
 * src/os/posix/ (process.c, paths.c, files.c, time.c, thread.c, machine.c,
 * registry.c, native.c, sjis.c, pipeproc.c, font.c) provide everything that
 * has nothing to do with the display, and the files of src/os/sdl/ here
 * provide the window, its input and the presentation:
 *
 *   window.c      the main window, the picture texture, the blits
 *   events.c      the event pump and the key / mouse mapping
 *   sys.c         the screen, the display modes, the cursor, the key state
 *   audio.c       the mixer's pull thread (silent for now), MCI, movies
 *   dialogs.c     the message boxes and the script dialogs
 *   unsupported.c child windows, the text entry, the debugger's window
 *
 * The picture the engine draws is the renderer's *logical* size
 * (SDL_RenderSetLogicalSize), so everything above this layer works in
 * picture pixels: a windowed client area of another size and full screen
 * only change how the renderer scales the picture into the window, and the
 * pointer coordinates are mapped back through the same transform.
 */
#ifndef BGI_OS_SDL_INTERNAL_H
#define BGI_OS_SDL_INTERNAL_H

#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_posix.h"

#include <SDL.h>

// ---- window.c: the main window, the picture, the blits ----------------------------------------

extern SDL_Window* gSdlWin;      // NULL before OS_WindowCreate and after OS_WindowDestroy
extern SDL_Renderer* gSdlRen;    // the renderer of that window
extern int gSdlAlive, gSdlShown; // created / mapped
extern int gSdlActive, gSdlMinimized, gSdlFullscreen;
extern int gSdlClientW, gSdlClientH; // the window's client size in pixels
extern int gSdlPicW, gSdlPicH;       // the picture the engine draws: the renderer's logical size
extern int gSdlCloseRequested, gSdlQuitPosted; // OS_WindowClose / OS_PostQuit, delivered by the next pump

// what OS_WindowGetDc hands out: a token naming the window (nothing is acquired)
typedef struct SdlDc
{
	int child; // slot index, -1 = the main window
} SdlDc_t;

extern SdlDc_t gSdlMainDc;

int Sdl_EnsureVideo(void);   // SDL_Init(SDL_INIT_VIDEO) once; 1 ok
void Sdl_ShutdownVideo(void); // SDL_Quit and forget what it set up
void Sdl_SyncClientSize(void); // gSdlClientW / H from the window
void Sdl_ApplyLogical(void);   // the logical size of the renderer, then black
void Sdl_FillBlack(void);      // OS_WindowFillBlack's body: clear the window black

// window client pixels -> picture pixels and back (the renderer's scaling and letterboxing)
void Sdl_WindowToPicture(float wx, float wy, int* px, int* py);
void Sdl_PictureToWindow(float px, float py, int* wx, int* wy);

// ---- events.c ---------------------------------------------------------------------------------

extern OsEventHandlers_t gSdlHandlers; // copied by OS_SetEventHandlers; every pointer is checked

#if defined(__ANDROID__)
/* The pointer of the touch layer, in picture pixels: the finger while it is
 * down (and while that gesture's click waits), or the row's hover point while
 * the corner button holds the row up; 0 = no pointer. */
int Sdl_TouchPointer(int* x, int* y);

// the corner button over the picture (window.c calls this after every full copy)
void Sdl_DrawTouchBar(SDL_Renderer* ren);
#endif

// ---- audio.c ----------------------------------------------------------------------------------

/* The app lifecycle: pause the output while the activity is in the
 * background, resume it on the way back. */
void Sdl_AudioSuspend(int suspended);

// ---- sys.c ------------------------------------------------------------------------------------

extern int gSdlScreenW, gSdlScreenH; // the desktop in pixels (OS_ScreenSize)
extern uint8_t gSdlKeys[256];        // GetAsyncKeyState emulation, indexed by virtual key

void Sdl_NoteKey(int vk, int down); // record a key or mouse button for OS_KeyState
void Sdl_CursorApply(void);         // the cursor from the shape and the show count

#endif // BGI_OS_SDL_INTERNAL_H
