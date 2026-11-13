/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dialogs.c - the SDL back end's message boxes, script dialogs and the
 *             launcher (inc/bgi/os.h)
 *
 * OS_MessageBox is the platform's own message box (SDL_ShowMessageBox), the
 * buttons and the icon of the MB_* flags kept; without a window - an error
 * before the window exists - the text goes to stderr and the default button
 * is returned, as the POSIX back end does without a display.
 *
 * The rest is not built yet: the script's input dialogs ("B0 8x"), the file
 * dialogs ("80 3B", "81 38"), the folder browser ("81 3A") and the
 * launcher's "Select game" window need widget code of their own, which Stage
 * B adds.  Until then a dialog reports what a back end without it reports:
 * cancelled, no file chosen, no launcher to show.  OS_LauncherDialog says -1
 * ("no display") rather than 0 ("quit"), so the caller does not think the
 * user chose to leave.
 */
#include "sdl_internal.h"

// ---- the message box ----------------------------------------------------------------------------

// the box's text (Shift-JIS) as the UTF-8 SDL takes
static void ToUtf8(const char* sjis, char* out, size_t n)
{
	OsPosix_SjisToUtf8(sjis ? sjis : "", out, n);
}

// the text on stderr, for a box that cannot be shown (before the window exists)
static void BoxToStderr(const char* text, const char* caption)
{
	char utf8[0x800];
	ToUtf8(caption, utf8, sizeof utf8);
	fprintf(stderr, "[%s] ", utf8);
	ToUtf8(text, utf8, sizeof utf8);
	fprintf(stderr, "%s\n", utf8);
}

/* MessageBox over the main window: modal; the id of the button pressed (the
 * close box counts as cancel).  MB_YESNO has no cancel button, as on Windows,
 * so its close box returns the default (No unless MB_DEFBUTTON2). */
int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	const char* btnText[2];
	int btnId[2], nBtn = 0, defBtn, result = 0;
	SDL_MessageBoxButtonData buttons[2];
	SDL_MessageBoxData data;
	char text8[0x800], caption8[0x400];
	uint32_t type = flags & 0xf, icon = flags & 0xf0; // the MB_* button and icon fields

	switch(type)
	{
		case OS_MB_OKCANCEL:
			btnText[0] = "OK", btnId[0] = OS_IDOK;
			btnText[1] = "Cancel", btnId[1] = OS_IDCANCEL;
			nBtn = 2;
			break;
		case OS_MB_YESNO:
			btnText[0] = "Yes", btnId[0] = OS_IDYES;
			btnText[1] = "No", btnId[1] = OS_IDNO;
			nBtn = 2;
			break;
		case OS_MB_RETRYCANCEL:
			btnText[0] = "Retry", btnId[0] = OS_IDRETRY;
			btnText[1] = "Cancel", btnId[1] = OS_IDCANCEL;
			nBtn = 2;
			break;
		default:
			btnText[0] = "OK", btnId[0] = OS_IDOK;
			nBtn = 1;
			break;
	}
	defBtn = (flags & OS_MB_DEFBUTTON2) && nBtn > 1 ? 1 : 0;

	if(!gSdlWin || getenv("BGI_MSGBOX_STDERR"))
	{ // headless (or asked to with BGI_MSGBOX_STDERR, for unattended test runs): say it on stderr, take the default button
		BoxToStderr(text, caption);
		return btnId[defBtn];
	}
	ToUtf8(caption, caption8, sizeof caption8);
	ToUtf8(text, text8, sizeof text8);
	memset(buttons, 0, sizeof buttons);
	memset(&data, 0, sizeof data);
	switch(icon)
	{ // the SDL icon kinds; MB_ICONHAND is the error box, MB_ICONQUESTION has none of its own
		case OS_MB_ICONHAND: data.flags = SDL_MESSAGEBOX_ERROR; break;
		case OS_MB_ICONWARNING: data.flags = SDL_MESSAGEBOX_WARNING; break;
		case OS_MB_ICONINFO:
		case OS_MB_ICONQUESTION: data.flags = SDL_MESSAGEBOX_INFORMATION; break;
		default: break;
	}
	{
		int i;
		for(i = 0; i < nBtn; i++)
		{
			buttons[i].flags = i == defBtn ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
			buttons[i].buttonid = btnId[i];
			buttons[i].text = btnText[i];
		}
		if(type != OS_MB_YESNO && nBtn > 1)
			buttons[defBtn == 0 ? 1 : 0].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
	}
	data.window = gSdlWin;
	data.title = caption8;
	data.message = text8;
	data.numbuttons = nBtn;
	data.buttons = buttons;
	if(SDL_ShowMessageBox(&data, &result) != 0 || result <= 0)
	{
		BoxToStderr(text, caption); // SDL could not put it on the screen after all
		return btnId[defBtn];
	}
	return result;
}

// ---- the script dialogs ("B0 8x") ----------------------------------------------------------------
// Not built yet: each reports a cancelled dialog, so the script carries on with what was there before.

static void WarnOnce(int* flag, const char* what)
{
	if(*flag)
		return;
	*flag = 1;
	fprintf(stderr, "sdl: %s is not implemented yet; it is reported as cancelled\n", what);
}

int OS_DialogInput1(const char* title, const char* initial, int maxLen, int digits, char* out)
{
	static int warned;
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	BGI_UNUSED(maxLen);
	BGI_UNUSED(digits);
	WarnOnce(&warned, "the script's input dialog");
	if(out)
		out[0] = 0;
	return 0;
}

int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, int digits1, char* out1,
	const char* label2, const char* initial2, int maxLen2, int digits2, char* out2, int compact)
{
	static int warned;
	BGI_UNUSED(title);
	BGI_UNUSED(label1);
	BGI_UNUSED(initial1);
	BGI_UNUSED(maxLen1);
	BGI_UNUSED(digits1);
	BGI_UNUSED(label2);
	BGI_UNUSED(initial2);
	BGI_UNUSED(maxLen2);
	BGI_UNUSED(digits2);
	BGI_UNUSED(compact);
	WarnOnce(&warned, "the script's two-field input dialog");
	if(out1)
		out1[0] = 0;
	if(out2)
		out2[0] = 0;
	return 0;
}

int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	static int warned;
	WarnOnce(&warned, "the profile dialog");
	if(surname)
		surname[0] = 0;
	if(givenName)
		givenName[0] = 0;
	if(nickname)
		nickname[0] = 0;
	if(pronoun)
		pronoun[0] = 0;
	if(month)
		*month = 0;
	if(day)
		*day = 0;
	return 0;
}

// ---- file and folder dialogs ---------------------------------------------------------------------

int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir,
	const char* title)
{
	static int warned;
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(filter);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	WarnOnce(&warned, "the file dialog");
	return 0;
}

int OS_FileDialogN(int save, char* path, size_t n, const char* const* descs, const char* const* pats, int count,
	const char* defExt, const char* initialDir, const char* title)
{
	static int warned;
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(descs);
	BGI_UNUSED(pats);
	BGI_UNUSED(count);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	WarnOnce(&warned, "the file dialog");
	return 0;
}

int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	static int warned;
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	WarnOnce(&warned, "the folder browser");
	return 0;
}

// ---- the launcher's "Select game" window ---------------------------------------------------------

int OS_LauncherDialog(OsLauncher_t* l)
{
	BGI_UNUSED(l);
	fprintf(stderr, "sdl: the launcher's window is not implemented yet; run the engine from the game's directory\n");
	return -1; // no display for it: not "quit"
}
