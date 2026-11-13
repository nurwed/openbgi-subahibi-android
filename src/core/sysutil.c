/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * sysutil.c - allocation helpers, the engine lock, the tick source, the
 *             idle sleep and the CRT random generator (inc/bgi/common.h
 *             for the allocation and random functions, inc/bgi/sys.h for
 *             the rest)
 */
#include "bgi/sys.h"
#include "bgi/os.h"
#include "bgi/display.h"

#if defined(__ANDROID__)
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "bgi/android_prof.h"

// ---- the start-up / load profiler (Android only, see android_prof.h) ------

static uint64_t gAndroidProfBaseUs; // AndroidProf_Begin: the process-start mark

/* The heavy per-open / per-read / per-blit / per-second lines are off
 * unless asked for: BGI_PROF in the environment (anything but empty or
 * "0"), or a file <BGI_FILES_DIR>/prof.txt that adb can push.  The answer
 * is cached, so a marker pushed while the game runs takes effect on the
 * next start.  Phases and failures ignore it (AndroidProf_Err). */
static int gAndroidProfOn = -1;

int AndroidProf_Enabled(void)
{
	if(gAndroidProfOn < 0)
	{
		const char* env = getenv("BGI_PROF");
		int on = env && *env && !(env[0] == '0' && !env[1]);
		if(!on)
		{
			const char* dir = getenv("BGI_FILES_DIR");
			char marker[0x400];
			struct stat st;
			if(dir && *dir && snprintf(marker, sizeof marker, "%s/prof.txt", dir) < (int)sizeof marker)
				on = stat(marker, &st) == 0;
		}
		gAndroidProfOn = on;
	}
	return gAndroidProfOn;
}

uint64_t AndroidProf_Us(void)
{
	struct timespec ts;
	if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

uint32_t AndroidProf_Ms(void)
{
	uint64_t now = AndroidProf_Us();
	return gAndroidProfBaseUs && now >= gAndroidProfBaseUs ? (uint32_t)((now - gAndroidProfBaseUs) / 1000) : 0;
}

// one fwrite: the loader/script/audio threads share stderr, and a line
// assembled in three stdio calls could otherwise interleave with theirs
static void ProfWrite(const char* fmt, va_list ap)
{
	char line[1024];
	uint32_t ms = AndroidProf_Ms();
	int n = snprintf(line, sizeof line, "prof: t=%u.%03u ", ms / 1000, ms % 1000);
	if(n < 0 || (size_t)n >= sizeof line)
		return;
	vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
	n = (int)strlen(line);
	if(n + 1 < (int)sizeof line)
	{
		line[n++] = '\n';
		line[n] = 0;
	}
	fwrite(line, 1, (size_t)n, stderr);
	fflush(stderr);
}

void AndroidProf_Log(const char* fmt, ...)
{
	va_list ap;
	if(!AndroidProf_Enabled())
		return;
	va_start(ap, fmt);
	ProfWrite(fmt, ap);
	va_end(ap);
}

void AndroidProf_Err(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	ProfWrite(fmt, ap);
	va_end(ap);
}

void AndroidProf_Begin(void)
{
	gAndroidProfBaseUs = AndroidProf_Us();
	fprintf(stderr, "prof: t=0.000 process start (monotonic %llu us)\n", (unsigned long long)gAndroidProfBaseUs);
	fflush(stderr);
}

void AndroidProf_Phase(const char* what)
{
	AndroidProf_Err("phase %s", what);
}

// the large allocations (BGI_LOAD_BUFFER is 128 MB) and their per-second count
static uint64_t gProfAllocSec;
static unsigned gProfAllocCount;
static uint64_t gProfAllocBytes;
static uint64_t gProfAllocUs;

static void ProfAlloc_Tick(uint64_t nowUs)
{
	uint64_t sec = nowUs / 1000000u;
	if(gProfAllocSec == 0)
	{
		gProfAllocSec = sec;
		return;
	}
	if(sec == gProfAllocSec)
		return;
	if(gProfAllocCount)
		AndroidProf_Log("alloc 1s: %u calls, %llu bytes, %llu us", gProfAllocCount,
			(unsigned long long)gProfAllocBytes, (unsigned long long)gProfAllocUs);
	gProfAllocSec = sec;
	gProfAllocCount = 0;
	gProfAllocBytes = 0;
	gProfAllocUs = 0;
}
#endif

// ---- allocation ----------------------------------------------------------

// malloc that never returns NULL: the process is aborted when memory runs out (n = 0 gives a 1-byte block)
void* BGI_Alloc(size_t n)
{
#if defined(__ANDROID__)
	uint64_t t0 = AndroidProf_Us();
#endif
	void* p = malloc(n ? n : 1);
	if(!p)
	{
		fprintf(stderr, "bgi: out of memory (%lu bytes)\n", (unsigned long)n);
		abort();
	}
#if defined(__ANDROID__)
	if(n >= ((size_t)4 << 20))
	{ // BGI_LOAD_BUFFER and the big decode buffers: mmap cost, and how often
		uint64_t dt = AndroidProf_Us() - t0;
		ProfAlloc_Tick(t0);
		gProfAllocCount++;
		gProfAllocBytes += n;
		gProfAllocUs += dt;
		AndroidProf_Log("alloc %lu B in %llu us%s", (unsigned long)n, (unsigned long long)dt, dt >= 2000 ? " SLOW" : "");
	}
#endif
	return p;
}

void* BGI_Calloc(size_t n)
{
#if defined(__ANDROID__)
	uint64_t t0 = AndroidProf_Us();
	uint64_t t1;
#endif
	void* p = BGI_Alloc(n);
	memset(p, 0, n);
#if defined(__ANDROID__)
	t1 = AndroidProf_Us();
	if(n >= ((size_t)4 << 20))
		AndroidProf_Log("calloc %lu B zeroed in %llu us", (unsigned long)n, (unsigned long long)(t1 - t0));
#endif
	return p;
}

void BGI_Free(void* p)
{
	free(p);
}

char* BGI_Strdup(const char* s)
{
	size_t n = strlen(s) + 1;
	char* d = (char*)BGI_Alloc(n);
	memcpy(d, s, n);
	return d;
}

//  --- engine lock ------------------------

/* The critical section held around the hash generator the DSC, SDC and
 * save-file codecs share, which the original's loader thread uses too.
 * Enter / Leave are no-ops before Init and after Delete. */
static OsLock_t* gEngineLock;

void EngineLock_Init(void)
{
	gEngineLock = OS_LockCreate();
}

// destroy the lock after waiting for whoever holds it
void EngineLock_Delete(void)
{
	OS_LockEnter(gEngineLock);
	OS_LockLeave(gEngineLock);
	OS_LockDestroy(gEngineLock);
	gEngineLock = NULL;
}

void EngineLock_Enter(void)
{
	if(gEngineLock)
		OS_LockEnter(gEngineLock);
}

void EngineLock_Leave(void)
{
	if(gEngineLock)
		OS_LockLeave(gEngineLock);
}

// ---- ticks ----------------------------------------------------------------

/* The original queries timeGetDevCaps and, when the minimum period is
 * below 10 ms, calls timeBeginPeriod(min) and switches GetTicks to
 * timeGetTime.  The OS layer always provides a millisecond clock, so the
 * switch is implicit here. */
static int gTimerPeriodOn; // between Timer_Begin and Timer_End

// raise the timer resolution (start-up)
void Timer_Begin(void)
{
	OS_TimerBeginPeriod();
	gTimerPeriodOn = 1;
}

// restore it (shutdown)
void Timer_End(void)
{
	if(gTimerPeriodOn)
	{
		OS_TimerEndPeriod();
		gTimerPeriodOn = 0;
	}
}

/* The engine's clock: milliseconds, wrapping at 2^32.  Up to 1.529 it is
 * the system clock.  From 1.535 on it can be stopped (Clock_Stop /
 * Clock_Resume): while stopped it reads the value it had when it stopped,
 * and the stopped time is taken out of it afterwards.  From 1.573 on it
 * does not see jumps either: a reading below the previous one, or more
 * than the step limit ("81 04", 500 ms by default) above it, moves the
 * base by the difference, so the clock starts at 0 on its first reading
 * and stands still across a stall such as a long load. */
static uint32_t gClockBase;          // subtracted from the system clock
static uint32_t gClockFrozen;        // the value while stopped; 0 while running (a clock stopped at 0 runs on)
static uint32_t gClockLast;          // the system clock at the previous reading (1.573 on)
static uint32_t gClockMaxStep = 500; // the longest step that counts as time passing
static int gClockStopInactive;       // "81 68": stop while the window is inactive
static int gClockStopped;            // 1.547 on: a stop is in effect (the next one is not counted)

uint32_t GetTicks(void)
{
	uint32_t now = OS_TicksMs();
	if(gEngine->gen < GEN_1_535)
		return now;
	if(gEngine->gen >= GEN_1_573)
	{
		if(now < gClockLast || gClockLast + gClockMaxStep < now)
			gClockBase += now - gClockLast;
		gClockLast = now;
	}
	return gClockFrozen ? gClockFrozen : now - gClockBase;
}

/* Stop the clock (the window lost the focus, or a message box or a
 * dialog is up).  `force` (1.547 on, the boxes and dialogs) stops it
 * whatever "81 68" says; the deactivation stops it only when "81 68" asked
 * for that.  From 1.547 on a second stop before the resume changes
 * nothing; from 1.588 on nothing happens before the window exists.  1 when
 * the clock was stopped here. */
int Clock_Stop(int force)
{
	int stopped = 0;
	if(gEngine->gen < GEN_1_535)
		return 0;
	if(gEngine->gen < GEN_1_547)
	{
		if(gClockStopInactive && !gClockFrozen)
		{
			gClockFrozen = GetTicks();
			return 1;
		}
		return 0;
	}
	if((gEngine->gen >= GEN_1_588 && !gWindowAlive) || gClockStopped)
		return 0;
	if((gClockStopInactive || force) && !gClockFrozen)
	{
		gClockFrozen = GetTicks();
		stopped = 1;
	}
	gClockStopped = 1;
	return stopped;
}

/* Let the clock run on, the stopped time taken out of it (1.535: only
 * while "81 68" is set).  1 when it had been stopped. */
int Clock_Resume(void)
{
	uint32_t at = gClockFrozen;
	if(gEngine->gen < GEN_1_535)
		return 0;
	if(gEngine->gen < GEN_1_547)
	{
		if(!gClockStopInactive || !at)
			return 0;
	}
	else
	{
		if((gEngine->gen >= GEN_1_588 && !gWindowAlive) || !gClockStopped)
			return 0;
		gClockStopped = 0;
		if(!at)
			return 0;
	}
	gClockFrozen = 0;
	gClockBase += GetTicks() - at;
	return 1;
}

// 1.547 on: a message box or a dialog is up - the clock stops whatever "81 68" says, and runs on after it
void Clock_BoxBegin(void)
{
	if(gEngine->gen >= GEN_1_547)
		Clock_Stop(1);
}

void Clock_BoxEnd(void)
{
	if(gEngine->gen >= GEN_1_547)
		Clock_Resume();
}

// "81 68" (1.535 on): stop the clock (and from 1.547 on the sound) while the window is inactive; the previous setting
int Clock_SetStopWhenInactive(int on)
{
	int prev = gClockStopInactive;
	gClockStopInactive = on;
	return prev;
}

int Clock_StopsWhenInactive(void)
{
	return gClockStopInactive;
}

// "81 04" (1.616 on): the step limit, 50 .. 60000 ms; 1 when accepted
int Clock_SetMaxStep(uint32_t ms)
{
	if(ms - 50 > 60000 - 50)
		return 0;
	gClockMaxStep = ms;
	return 1;
}

// ---- idle sleep ------------------------------------------------------------

void Idle_Init(void)
{
	OS_IdleInit();
}

void Idle_Shutdown(void)
{
	OS_IdleShutdown();
}

// the ~0.5 ms sleep at the end of a pass that presented nothing (Display_FrameIdle)
void Sys_Idle(void)
{
	OS_IdleSleep();
}

// ---- the CRT random generator -----------------------------------------------------------
static uint32_t gRandSeed = 1; // the CRT's holdrand, initially 1

// "80 00": srand
void BGI_Srand(uint32_t seed)
{
	gRandSeed = seed;
}

// "80 01" / "80 02": rand, the MSVC sequence (seed * 214013 + 2531011, bits 16 .. 30)
int BGI_Rand(void)
{
	gRandSeed = gRandSeed * 214013u + 2531011u;
	return (int)((gRandSeed >> 16) & 0x7fffu);
}
