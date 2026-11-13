/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the SDL back end's sound output, CD audio and movies
 *           (inc/bgi/os.h)
 *
 * The sound library mixes in software and pulls 16-bit stereo frames
 * through OS_AudioStart()'s callback: the back end calls
 * fill(user, samples, frames) from its audio thread whenever it needs
 * `frames` interleaved stereo frames, and the mixer fills every one of them.
 * Here that thread is SDL's: an SDL_OpenAudioDevice in 44.1 kHz, 16-bit
 * stereo, with periods of about 20 ms (rate / 50, the period the POSIX back
 * end's ALSA path uses) and the mixer's own callback as the device callback.
 * For testing, BGI_AUDIO_FRAMES overrides the device period in frames
 * (clamped to 256 .. 8192); the silent clock below keeps its 20 ms period.
 * The contract is the POSIX one: the callback never runs on the engine's
 * thread, and no callback runs after OS_AudioStop returns.
 *
 * A machine without a sound device is not an error: when SDL cannot open
 * one (no card, a VM, a busy default device) the mixer is run against the
 * clock instead - the same 20 ms periods, the frames dropped - and one line
 * on stderr says so, exactly as the POSIX back end does without ALSA.  The
 * game plays silently rather than not at all.
 *
 * The engine calls OS_AudioStop from its shutdown (Sound_Shutdown), which
 * runs after the window was destroyed and after this back end called
 * SDL_Quit; SDL closes its devices there, so the stop path checks that the
 * audio subsystem is still up before it touches the device.
 *
 * CD audio through MCI, PlaySound and the DirectShow movie graph have no
 * counterpart here: every call reports failure, which the engine handles (a
 * CD that is not there; a movie, which src/sys/moviefx.c then plays through
 * an ffmpeg pipe).
 */
#include "sdl_internal.h"

#include <pthread.h>
#include <time.h>
#include <errno.h>

#if defined(__ANDROID__)
#include "bgi/android_prof.h"
#endif

static struct
{
	int running;        // between OS_AudioStart and OS_AudioStop
	int rate;           // frames per second
	OsAudioFill_t fill; // the mixer's callback and its argument
	void* user;
	SDL_AudioDeviceID dev; // the open device; 0 = the silent clock below
	pthread_t thread;      // the silent clock's thread (dev == 0)
	volatile int stop;     // set by OS_AudioStop: the thread ends after its current period
#if defined(__ANDROID__)
	Uint32 cbLastTicks; // SDL_GetTicks of the last device callback, 0 = none yet
	Uint32 cbCount;     // device callbacks since the device was opened
	int periodMs;       // the obtained spec's period, in ms
#endif
} gAudio;

#if defined(__ANDROID__)
// one line per device-lifecycle call
static void Android_LogAudio(const char* what)
{
	fprintf(stderr, "sdl: android: audio %s at %u ms, %u callbacks, dev %u\n", what, (unsigned)SDL_GetTicks(),
		(unsigned)gAudio.cbCount, (unsigned)gAudio.dev);
	fflush(stderr);
}
#endif

// nanosleep that resumes after a signal
static void SleepNs(long ns)
{
	struct timespec ts;
	ts.tv_sec = ns / 1000000000L;
	ts.tv_nsec = ns % 1000000000L;
	while(nanosleep(&ts, &ts) == -1 && errno == EINTR)
	{
	}
}

/* The device callback (SDL's audio thread): SDL asks for `len` bytes of
 * interleaved 16-bit stereo, which is len / 4 frames; the buffer is zeroed
 * first because the mixer fills every frame and must not read SDL's. */
static void SdlAudioCallback(void* user, Uint8* stream, int len)
{
	int frames = len / (int)(2 * sizeof(int16_t));
#if defined(__ANDROID__)
	{
		/* a gap over two periods means SDL stopped pulling: an underrun, a
		 * pause or a device restart */
		Uint32 now = SDL_GetTicks();
		Uint32 limit = (Uint32)(gAudio.periodMs * 2);
		if(limit < 2)
			limit = 2;
		if(gAudio.cbLastTicks != 0 && now - gAudio.cbLastTicks > limit && AndroidProf_Enabled())
			fprintf(stderr, "sdl: android: audio callback gap %u ms (period %d ms) at %u ms, %d frames, %u callbacks\n",
				(unsigned)(now - gAudio.cbLastTicks), gAudio.periodMs, (unsigned)now, frames,
				(unsigned)gAudio.cbCount);
		gAudio.cbLastTicks = now;
		gAudio.cbCount++;
	}
#endif
	memset(stream, 0, (size_t)len);
	gAudio.fill(user, (int16_t*)stream, frames);
}

/* The silent clock: one 20 ms period at a time, filled by the mixer and
 * dropped, so that fades, loops and the "is it still playing" bookkeeping
 * keep their timing on a machine without a sound device. */
static void* AudioThread(void* arg)
{
	int period = gAudio.rate / 50; // 20 ms
	int16_t* buf;
	BGI_UNUSED(arg);
	if(period <= 0)
		period = 1;
	buf = (int16_t*)malloc((size_t)period * 2 * sizeof(int16_t));
	if(!buf)
		return NULL;
	while(!gAudio.stop)
	{
		memset(buf, 0, (size_t)period * 2 * sizeof(int16_t));
		gAudio.fill(gAudio.user, buf, period);
		SleepNs(20L * 1000000L);
	}
	free(buf);
	return NULL;
}

/* Open the playback device for `want` exactly (no rate, format or channel
 * conversion: the mixer's rate is the device's).  SDL starts the device
 * paused, so the first callback comes only once OS_AudioStart unpauses it,
 * after gAudio is filled in.  0 when the subsystem or the device cannot be
 * had; *err gets SDL's reason. */
static int OpenDevice(SDL_AudioSpec* want, const char** err)
{
	SDL_AudioSpec have;
	if(!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		*err = SDL_GetError();
		return 0;
	}
	gAudio.dev = SDL_OpenAudioDevice(NULL, 0, want, &have, 0);
	if(!gAudio.dev)
	{
		*err = SDL_GetError();
		return 0;
	}
	if(have.freq != want->freq || have.format != want->format || have.channels != want->channels)
	{ // SDL was asked for no changes and must not have made any: rather silence than the wrong speed
		SDL_CloseAudioDevice(gAudio.dev);
		gAudio.dev = 0;
		*err = "SDL returned another sample format";
		return 0;
	}
#if defined(__ANDROID__)
	gAudio.periodMs = (int)((long long)have.samples * 1000 / (have.freq > 0 ? have.freq : 1));
	gAudio.cbLastTicks = 0;
	gAudio.cbCount = 0;
	{
		const char* drv = SDL_GetCurrentAudioDriver();
		fprintf(stderr,
			"sdl: android: audio open driver %s: asked %d Hz 0x%04x %d ch %u frames, got %d Hz 0x%04x %d ch %u frames "
			"(period %d ms)\n",
			drv && *drv ? drv : "(none)", want->freq, (unsigned)want->format, (int)want->channels,
			(unsigned)want->samples, have.freq, (unsigned)have.format, (int)have.channels, (unsigned)have.samples,
			gAudio.periodMs);
		fflush(stderr);
	}
#endif
	if(getenv("BGI_AUDIO_DEBUG"))
		fprintf(stderr, "sdl: audio device opened: %d Hz, 16-bit stereo, %u frames per period\n", have.freq,
			(unsigned)have.samples);
	return 1;
}

/* The device period in frames: rate / 50 (about 20 ms, the period the POSIX
 * back end uses), or BGI_AUDIO_FRAMES when that names an integer, clamped to
 * 256 .. 8192 so that testing cannot ask SDL for an absurd buffer. */
static int AudioFrames(int rate)
{
	const char* s = getenv("BGI_AUDIO_FRAMES");
	int frames = rate / 50;
	if(frames < 1)
		frames = 1;
	if(s && *s)
	{
		char* end;
		long v = strtol(s, &end, 10);
		if(end != s && *end == 0)
		{
			if(v < 256)
				v = 256;
			if(v > 8192)
				v = 8192;
			frames = (int)v;
		}
		else
			fprintf(stderr, "sdl: BGI_AUDIO_FRAMES \"%s\" is not an integer, using the default\n", s);
	}
	return frames;
}

/* Open the device and start the mixer; without one the silent clock runs.
 * 0 when running already, for a bad argument, or when neither the device nor
 * a thread could be had. */
int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	SDL_AudioSpec want;
	const char* err = NULL;
	if(gAudio.running || rate <= 0 || !fill)
		return 0;
	gAudio.rate = rate;
	gAudio.fill = fill;
	gAudio.user = user;
	gAudio.dev = 0;
	gAudio.stop = 0;
	memset(&want, 0, sizeof want);
	want.freq = rate;
	want.format = AUDIO_S16SYS; // 16-bit signed, the machine's byte order
	want.channels = 2;         // interleaved stereo
	want.samples = (Uint16)AudioFrames(rate); // about 20 ms, or BGI_AUDIO_FRAMES for testing
	want.callback = SdlAudioCallback;
	want.userdata = user;
	if(OpenDevice(&want, &err))
	{
		gAudio.running = 1;
#if defined(__ANDROID__)
		Android_LogAudio("resume (start)");
#endif
		SDL_PauseAudioDevice(gAudio.dev, 0); // start pulling
		return 1;
	}
	// no device: the mixer still runs, silently, against the clock
	fprintf(stderr, "sdl: no audio device (%s); the mixer runs against the clock, the game is silent\n",
		err && *err ? err : "unknown error");
	if(pthread_create(&gAudio.thread, NULL, AudioThread, NULL) != 0)
		return 0;
	gAudio.running = 1;
	return 1;
}

// stop the device (or the clock thread): joined, so that no callback runs after this returns
void OS_AudioStop(void)
{
	if(!gAudio.running)
		return;
	if(gAudio.dev)
	{
		/* SDL_Quit has already closed the device when the engine destroyed
		 * the window before shutting the sound library down: there is
		 * nothing left to stop then */
		if(SDL_WasInit(SDL_INIT_AUDIO))
		{
#if defined(__ANDROID__)
			Android_LogAudio("pause (stop)");
#endif
			SDL_PauseAudioDevice(gAudio.dev, 1);
			SDL_CloseAudioDevice(gAudio.dev); // waits for a callback that is running
		}
		gAudio.dev = 0;
	}
	else
		pthread_join(gAudio.thread, NULL);
	gAudio.running = 0;
}

/* SDL_APP_WILLENTERBACKGROUND / SDL_APP_DIDENTERFOREGROUND: the device stops
 * pulling while the activity is away and starts again afterwards. */
void Sdl_AudioSuspend(int suspended)
{
#if defined(__ANDROID__)
	/* log every request, including repeats: they explain the callback gaps */
	fprintf(stderr, "sdl: android: audio suspend request %d at %u ms (running %d, dev %u)\n", suspended,
		(unsigned)SDL_GetTicks(), gAudio.running, (unsigned)gAudio.dev);
	fflush(stderr);
#endif
	if(!gAudio.running || !gAudio.dev)
		return;
	if(!SDL_WasInit(SDL_INIT_AUDIO))
		return;
#if defined(__ANDROID__)
	Android_LogAudio(suspended ? "pause (suspend)" : "resume (suspend)");
#endif
	SDL_PauseAudioDevice(gAudio.dev, suspended ? 1 : 0);
}

// the mixer can always run (on the device, or silently against the clock)
int OS_AudioAvailable(void)
{
	return 1;
}

// ---- CD audio (MCI) ---------------------------------------------------------------
// No CD audio: every call fails, so the engine sees a drive without a CD.

uint32_t OS_MciOpenCdAudio(const char* element)
{
	BGI_UNUSED(element);
	return 0;
}

int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciClose(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(item);
	BGI_UNUSED(out);
	return 0;
}

int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(from);
	BGI_UNUSED(to);
	BGI_UNUSED(notify);
	return 0;
}

int OS_MciStop(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

// PlaySound: not accepted
int OS_PlaySoundFile(const char* path)
{
	BGI_UNUSED(path);
	return 0;
}

// ---- movies ----------------------------------------------------------------------
// No movie playback: the open fails, and the rest reports a graph that is not there.

int OS_MovieOpen(const char* path)
{
	BGI_UNUSED(path);
	return OS_MOVIE_FAILED;
}

int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(volume);
	BGI_UNUSED(lengthMs);
	return 0;
}

void OS_MovieStop(void)
{
}

int OS_MovieIsRunning(void)
{
	return 0;
}

void OS_MovieSetVolume(int32_t vol)
{
	BGI_UNUSED(vol);
}
