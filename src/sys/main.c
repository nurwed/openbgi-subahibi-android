/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * main.c - the process entry point
 *
 * The original is WinMain; Engine_Main in engine.c (bgi/engine.h) is that
 * function without the Windows glue.  This file only turns the host's
 * entry point into a call of Engine_Main with the command line as one
 * string, which the engine parses itself (Engine_Main strips the
 * reimplementation's "--" options, Boot_ParseCmdLine understands the
 * original's switches, e.g. "Execute as a launcher.").
 */
#include "bgi/engine.h"
#include "bgi/os.h"

#if defined(__ANDROID__)
#include <SDL.h> // redefines main to SDL_main, which SDLActivity starts (see below)
#include <stdio.h>
#include <stdlib.h>
#include "bgi/android_prof.h"
#endif

#if defined(BGI_WIN32)
#include "bgi/os_win32.h" // windows.h, and the back end's command line

/* the Windows entry point.  WinMain's own command line is in the system's
 * code page, which may not be able to spell a path; the back end converts
 * the process's wide one instead (OsWin32_CommandLine: without the
 * program name, in the engine's encoding). */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show)
{
	char cmd[0x1000];
	int r;
	BGI_UNUSED(hInst);
	BGI_UNUSED(hPrev);
	BGI_UNUSED(cmdLine);
	BGI_UNUSED(show);
	if(!OS_Init())
		return -1;
	OsWin32_CommandLine(cmd, sizeof cmd);
	r = Engine_Main(cmd);
	OS_Shutdown();
	return r;
}

#else

#if defined(__ANDROID__)
/* The Java side starts this main as SDL_main (SDL_main.h renames it under
 * __ANDROID__); SDL's nativeRunMain builds argv from
 * SDLActivity.getArguments(), so argv[1] is whatever the activity passed.
 *
 * Android sets no environment for a process started this way, so the two
 * directories are filled in here, before OS_Init (src/os/posix/process.c)
 * reads them: the app's writable directory from SDL (getFilesDir(), where
 * the configuration, registry and saves go) unless BGI_FILES_DIR is set,
 * and BGI_GAME_DIR from argv[1] unless that is set.
 *
 * stderr and stdout then go to BGI_FILES_DIR/bgi.log, since a device shows
 * nothing of the process's own output.  "a" appends and line buffering
 * keeps the lines up to a crash. */
static void Android_InitEnv(int argc, char** argv)
{
	const char* files;
	const char* game;
	char log[0x400];
	if(!getenv("BGI_FILES_DIR"))
	{
		files = SDL_AndroidGetInternalStoragePath();
		if(files && *files)
			setenv("BGI_FILES_DIR", files, 0);
	}
	if(argc > 1 && argv[1] && argv[1][0] && !getenv("BGI_GAME_DIR"))
		setenv("BGI_GAME_DIR", argv[1], 0);
	files = getenv("BGI_FILES_DIR");
	if(files && *files)
	{
		snprintf(log, sizeof log, "%s/bgi.log", files);
		if(freopen(log, "a", stderr))
			setvbuf(stderr, NULL, _IOLBF, 0);
		if(freopen(log, "a", stdout))
			setvbuf(stdout, NULL, _IOLBF, 0);
	}
	game = getenv("BGI_GAME_DIR");
	fprintf(stderr, "bgi: android: argc=%d argv[1]=%s BGI_GAME_DIR=%s BGI_FILES_DIR=%s\n", argc,
		argc > 1 && argv[1] && argv[1][0] ? argv[1] : "(none)",
		game && *game ? game : "(none)", files && *files ? files : "(none)");
}
#endif

/* the POSIX entry point: the arguments are rejoined into one command line,
 * as GetCommandLine would give it without the program name - joined by
 * single spaces, an argument with a space or a tab in it (a game directory
 * "VN/[August] Fortune Arterial", Do not use mutex.) in double quotes
 * unless it already is in them.  Arguments that do not fit the buffer are
 * dropped. */
int main(int argc, char** argv)
{
	char cmdLine[0x1000];
	size_t len = 0;
	int i, r;
#if defined(__ANDROID__)
	Android_InitEnv(argc, argv); // must come before OS_Init
	AndroidProf_Begin();         // the log file is open: t = 0 here
#endif
	cmdLine[0] = 0;
	for(i = 1; i < argc; i++)
	{
		const char* a = argv[i];
		size_t n = strlen(a);
		int quote = strpbrk(a, " \t") != NULL && !(n >= 2 && a[0] == '"' && a[n - 1] == '"');
		if(len + n + 4 >= sizeof cmdLine)
			break;
		if(len)
			cmdLine[len++] = ' ';
		if(quote)
			cmdLine[len++] = '"';
		memcpy(cmdLine + len, a, n);
		len += n;
		if(quote)
			cmdLine[len++] = '"';
		cmdLine[len] = 0;
	}
	OS_SetArgv0(argv[0]);
	if(!OS_Init())
	{
#if defined(__ANDROID__)
		/* without the signal handlers and the configuration directory
		 * the engine cannot start; this line ends up in bgi.log */
		fprintf(stderr, "bgi: OS_Init failed, exiting\n");
#endif
		return -1;
	}
#if defined(__ANDROID__)
	AndroidProf_Phase("OS_Init done");
#endif
	r = Engine_Main(cmdLine);
	OS_Shutdown();
#if defined(__ANDROID__)
	AndroidProf_Phase("OS_Shutdown done");
#endif
	return r;
}

#endif
