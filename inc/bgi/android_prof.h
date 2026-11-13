/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * android_prof.h - timestamps for the Android start-up / load profiling
 *
 * Diagnostic only, Android only: nothing here exists in the desktop builds
 * (the include is inside "defined(__ANDROID__)" at every use), so their
 * preprocessed output is unchanged.  The implementation is in
 * src/core/sysutil.c; every line goes to stderr, which main.c points at
 * BGI_FILES_DIR/bgi.log.
 *
 * AndroidProf_Begin() starts the relative clock (main.c, after the log
 * file is open); AndroidProf_Ms() is the time since then, AndroidProf_Us()
 * the raw monotonic counter for measuring durations.
 *
 * The per-open / per-read / per-blit / per-second lines are off by default
 * so that a normal session leaves a small bgi.log: AndroidProf_Log does
 * nothing unless BGI_PROF is set (to anything but "0") or the file
 * <BGI_FILES_DIR>/prof.txt exists.  Phases and failures use
 * AndroidProf_Err, which is always written.
 */
#ifndef BGI_ANDROID_PROF_H
#define BGI_ANDROID_PROF_H

/*
 * No not-compiled stub on purpose: the callers guard with
 * #if defined(__ANDROID__), so the header is only ever seen on Android.
 */
#ifdef __ANDROID__

#include <stdint.h>

uint64_t AndroidProf_Us(void);            // monotonic microseconds
uint32_t AndroidProf_Ms(void);            // milliseconds since AndroidProf_Begin
void AndroidProf_Begin(void);             // t = 0: "process start"
void AndroidProf_Phase(const char* what); // a named point in the start-up (always written)
void AndroidProf_Log(const char* fmt, ...); // "prof: t=1.234 <fmt>"; off unless BGI_PROF / prof.txt
void AndroidProf_Err(const char* fmt, ...); // the same line, always written (failures)
int AndroidProf_Enabled(void);              // whether AndroidProf_Log writes anything

#endif /* __ANDROID__ */

#endif /* BGI_ANDROID_PROF_H */
