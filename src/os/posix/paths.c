/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * paths.c - Windows paths on a POSIX file system (inc/bgi/os_posix.h)
 *
 * Paths arrive as the engine builds them (Shift-JIS, '\\' separators,
 * optionally a "C:\" drive).  OsPosix_Path() turns them into UTF-8 POSIX
 * paths under the game directory, and when a path does not exist as
 * spelled it looks for a case-insensitive match component by component,
 * because the game data was written for a case-insensitive file system.
 * Every file service of files.c goes through it, so the engine never
 * sees a POSIX path.
 */
#include "posix_internal.h"

#if defined(__ANDROID__)
#include "bgi/android_prof.h"

/* Per-second sums of the path translation, plus a line for slow ones.
 * "rescans" counts the readdir() passes that were still needed, "cached" the
 * lookups answered from a cached listing ("neg" of those found nothing). */
static struct
{
	uint64_t sec;
	unsigned calls, comps, stats;
	unsigned rescans, rescanEntries, rescanSlow;
	unsigned cacheHits, cacheNeg;
	uint64_t pathUs, rescanUs, cacheUs;
} gProfPath;

static void ProfPath_Tick(uint64_t nowUs)
{
	uint64_t sec = nowUs / 1000000u;
	if(gProfPath.sec == 0)
	{
		gProfPath.sec = sec;
		return;
	}
	if(sec == gProfPath.sec)
		return;
	if(gProfPath.calls)
		AndroidProf_Log(
			"path 1s: calls=%u (%llu us) components=%u stats=%u rescans=%u (%u entries, %llu us, %u slow) cached=%u (%u neg, %llu us)",
			(unsigned)gProfPath.calls, (unsigned long long)gProfPath.pathUs, (unsigned)gProfPath.comps,
			(unsigned)gProfPath.stats, (unsigned)gProfPath.rescans, (unsigned)gProfPath.rescanEntries,
			(unsigned long long)gProfPath.rescanUs, (unsigned)gProfPath.rescanSlow, (unsigned)gProfPath.cacheHits,
			(unsigned)gProfPath.cacheNeg, (unsigned long long)gProfPath.cacheUs);
	gProfPath.sec = sec;
	memset(&gProfPath, 0, sizeof gProfPath);
	gProfPath.sec = sec;
}

/* ---- the directory listing cache ----
 *
 * MatchCase reads a whole directory to find one name, and the loader asks
 * for a loose file before every archive member, so a failed lookup would
 * read the game directory each time.  Each directory's listing is kept,
 * keyed by its resolved path; a lookup is a strcasecmp() pass and a miss is
 * cached too.  A write in the directory drops its record.  Directories that
 * cannot be opened or are too large are read per lookup as before.
 */
#define PATHCACHE_DIRS       16u     // directories kept (the least recently used is dropped)
#define PATHCACHE_DIR_NAMES  0x10000u // a directory with more entries is not cached
#define PATHCACHE_TOTAL_NAMES 0x20000u // entries held across all directories, at most

typedef struct PathCacheDir
{
	char* path;      // the cache key: the resolved directory, "." for the current one
	char** name;     // the entries, NUL-terminated, in readdir() order
	uint32_t count;  // entries in name
	uint64_t use;    // gPathCacheClock at the last lookup: the eviction order
} PathCacheDir;

static PathCacheDir gPathCache[PATHCACHE_DIRS];
static unsigned gPathCacheDirs;    // records in use
static unsigned gPathCacheNames;   // entries held, for PATHCACHE_TOTAL_NAMES
static uint64_t gPathCacheClock;   // tick of the last lookup
static pthread_mutex_t gPathCacheLock = PTHREAD_MUTEX_INITIALIZER;

static void PathCache_Release(PathCacheDir* d) // free the memory of a record (temporary or slot)
{
	uint32_t i;
	for(i = 0; i < d->count; i++)
		free(d->name[i]);
	free(d->name);
	free(d->path);
	memset(d, 0, sizeof *d);
}

static void PathCache_Drop(PathCacheDir* d) // free a slot and account for it
{
	gPathCacheNames -= d->count;
	if(gPathCacheDirs)
		gPathCacheDirs--;
	PathCache_Release(d);
}

// the record of `key`, or NULL
static PathCacheDir* PathCache_Find(const char* key)
{
	unsigned i;
	for(i = 0; i < PATHCACHE_DIRS; i++)
		if(gPathCache[i].path && strcmp(gPathCache[i].path, key) == 0)
			return &gPathCache[i];
	return NULL;
}

// an empty slot, evicting the least recently used record when all are in use
static PathCacheDir* PathCache_New(void)
{
	PathCacheDir* d = NULL;
	unsigned i;
	if(gPathCacheDirs < PATHCACHE_DIRS)
	{
		for(i = 0; i < PATHCACHE_DIRS; i++)
			if(!gPathCache[i].path)
			{
				d = &gPathCache[i];
				break;
			}
	}
	else
	{
		for(i = 0; i < PATHCACHE_DIRS; i++)
			if(!d || gPathCache[i].use < d->use)
				d = &gPathCache[i];
		PathCache_Drop(d);
	}
	if(!d)
		return NULL;
	gPathCacheDirs++;
	return d;
}

/* one readdir() pass into the temporary record `d`; 0 (and nothing kept) when
 * the directory cannot be opened, is too large or memory runs out */
static int PathCache_Load(PathCacheDir* d, const char* key)
{
	DIR* h = opendir(key[0] ? key : ".");
	struct dirent* e;
	uint32_t cap = 0;
	if(!h)
		return 0;
	d->path = strdup(key[0] ? key : ".");
	if(!d->path)
	{
		closedir(h);
		PathCache_Release(d);
		return 0;
	}
	while((e = readdir(h)) != NULL)
	{
		if(d->count == PATHCACHE_DIR_NAMES || gPathCacheNames + d->count == PATHCACHE_TOTAL_NAMES)
		{
			closedir(h);
			PathCache_Release(d);
			return 0;
		}
		if(d->count == cap)
		{
			uint32_t ncap = cap ? cap * 2 : 64;
			char** nn = (char**)realloc(d->name, (size_t)ncap * sizeof *nn);
			if(!nn)
			{
				closedir(h);
				PathCache_Release(d);
				return 0;
			}
			d->name = nn;
			cap = ncap;
		}
		d->name[d->count] = strdup(e->d_name);
		if(!d->name[d->count])
		{
			closedir(h);
			PathCache_Release(d);
			return 0;
		}
		d->count++;
	}
	closedir(h);
	return 1;
}

/* the record of `key`, reading the directory when it is not cached; NULL when
 * it cannot be cached, and the caller falls back to a readdir() */
static PathCacheDir* PathCache_Get(const char* key)
{
	PathCacheDir tmp;
	PathCacheDir* d = PathCache_Find(key);
	uint64_t t0, dt;
	if(d)
	{
		d->use = ++gPathCacheClock;
		return d;
	}
	memset(&tmp, 0, sizeof tmp);
	t0 = AndroidProf_Us();
	if(!PathCache_Load(&tmp, key))
		return NULL;
	dt = AndroidProf_Us() - t0;
	if(gPathCacheNames + tmp.count > PATHCACHE_TOTAL_NAMES || !(d = PathCache_New()))
	{
		PathCache_Release(&tmp);
		return NULL;
	}
	*d = tmp;
	gPathCacheNames += d->count;
	d->use = ++gPathCacheClock;
	AndroidProf_Log("path cache %s: %u entries in %llu us", d->path, d->count, (unsigned long long)dt);
	return d;
}

/* drop the cached listing of `posixDir`; files.c calls this after every write */
void OsPosix_PathCacheDrop(const char* posixDir)
{
	PathCacheDir* d;
	if(!posixDir || !*posixDir)
		return;
	pthread_mutex_lock(&gPathCacheLock);
	d = PathCache_Find(posixDir);
	if(d)
		PathCache_Drop(d);
	pthread_mutex_unlock(&gPathCacheLock);
}

// drop the listing of the directory that holds `posixPath`
void OsPosix_PathCacheDropPath(const char* posixPath)
{
	char dir[PATH_MAX];
	const char* slash;
	size_t n;
	if(!posixPath || !*posixPath)
		return;
	slash = strrchr(posixPath, '/');
	if(!slash)
	{
		OsPosix_PathCacheDrop("."); // a bare relative name
		return;
	}
	n = (size_t)(slash - posixPath);
	if(n == 0)
	{
		OsPosix_PathCacheDrop("/");
		return;
	}
	if(n >= sizeof dir)
		return;
	memcpy(dir, posixPath, n);
	dir[n] = 0;
	OsPosix_PathCacheDrop(dir);
}
#endif

/* one entry of `dir` ("" = the current directory) that matches `name`
 * without regard to ASCII case; `name` is replaced in place by the entry's
 * spelling when a match exists (the first one readdir yields) */
static void MatchCase(const char* dir, char* name)
{
#if defined(__ANDROID__)
	/* the cached listing first: a memory scan that keeps readdir()'s order */
	{
		const char* key = dir[0] ? dir : ".";
		PathCacheDir* pc;
		uint32_t i;
		uint64_t t0 = AndroidProf_Us();
		uint64_t dt;

		pthread_mutex_lock(&gPathCacheLock);
		pc = PathCache_Get(key);
		if(pc)
		{
			for(i = 0; i < pc->count; i++)
			{
				if(strcasecmp(pc->name[i], name) == 0)
				{
					strcpy(name, pc->name[i]);
					break;
				}
			}
			if(i == pc->count)
				gProfPath.cacheNeg++; // not there: the cached negative result
			gProfPath.cacheHits++;
			dt = AndroidProf_Us() - t0;
			gProfPath.cacheUs += dt;
		}
		pthread_mutex_unlock(&gPathCacheLock);
		if(pc)
			return; // answered from the cache
	}
#endif

	DIR* d = opendir(dir[0] ? dir : ".");
	struct dirent* e;
#if defined(__ANDROID__)
	uint64_t t0 = AndroidProf_Us();
	unsigned entries = 0;
	uint64_t dt;
#endif
	if(!d)
		return;
	while((e = readdir(d)) != NULL)
	{
#if defined(__ANDROID__)
		entries++;
#endif
		if(strcasecmp(e->d_name, name) == 0)
		{
			strcpy(name, e->d_name);
			break;
		}
	}
	closedir(d);
#if defined(__ANDROID__)
	dt = AndroidProf_Us() - t0;
	gProfPath.rescans++;
	gProfPath.rescanEntries += entries;
	gProfPath.rescanUs += dt;
	if(dt >= 5000)
	{
		gProfPath.rescanSlow++;
		AndroidProf_Log("path case scan %s for %s: %u entries, %llu us", dir[0] ? dir : ".", name, entries,
			(unsigned long long)dt);
	}
#endif
}

/* Shift-JIS Windows path -> UTF-8 POSIX path (into out, n bytes).  A drive
 * letter (any letter) maps to the executable's directory ("C:\" is where
 * the game lives), a leading separator to the file system root; '\\'
 * becomes '/'; every component that does not exist as spelled is replaced
 * by a case-insensitive match when there is one.  A relative path stays
 * relative (to the current directory); an empty one becomes "." or "/". */
void OsPosix_Path(const char* win, char* out, size_t n)
{
	char utf[PATH_MAX * 2];
	char cur[PATH_MAX]; // the POSIX path built so far
	const char* p;
	size_t o = 0;
	int absolute = 0;
#if defined(__ANDROID__)
	uint64_t t0 = AndroidProf_Us();
	unsigned comps = 0;
#endif

	OsPosix_SjisToUtf8(win, utf, sizeof utf);
	p = utf;
	if(((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
	{
		p += 2;
		if(*p == '\\' || *p == '/')
			p++;
		snprintf(cur, sizeof cur, "%s", gPosixExeDir); // the drive root is the game directory
		o = strlen(cur);
		if(o && cur[o - 1] == '/')
			cur[--o] = 0;
		absolute = 1;
	}
	else if(*p == '\\' || *p == '/')
	{
		cur[0] = 0;
		absolute = 1;
		p++;
	}
	else
		cur[0] = 0;
	BGI_UNUSED(absolute);

	while(*p)
	{ // one component at a time: probe it as spelled, then look for another spelling
		char comp[PATH_MAX];
		size_t c = 0;
		struct stat st;
		while(*p && *p != '\\' && *p != '/' && c + 1 < sizeof comp)
			comp[c++] = *p++;
		comp[c] = 0;
		while(*p == '\\' || *p == '/')
			p++;
		if(c == 0)
			continue;
#if defined(__ANDROID__)
		comps++;
#endif
		if(strcmp(comp, ".") != 0 && strcmp(comp, "..") != 0)
		{
			char probe[PATH_MAX];
			const char* dir = cur[0] ? cur : absolute ? "/"
													  : ".";
			snprintf(probe, sizeof probe, "%s%s%s", cur, cur[0] || absolute ? "/" : "", comp);
#if defined(__ANDROID__)
			{
				uint64_t s0 = AndroidProf_Us();
				int r = stat(probe, &st);
				uint64_t dt = AndroidProf_Us() - s0;
				gProfPath.stats++;
				if(dt >= 5000)
					AndroidProf_Log("path stat %s %llu us", probe, (unsigned long long)dt);
				if(r != 0)
					MatchCase(dir, comp);
			}
#else
			if(stat(probe, &st) != 0)
				MatchCase(dir, comp);
#endif
		}
		snprintf(cur + strlen(cur), sizeof cur - strlen(cur), "%s%s", cur[0] || absolute ? "/" : "", comp);
	}
	if(!cur[0])
		strcpy(cur, absolute ? "/" : ".");
	snprintf(out, n, "%s", cur);
#if defined(__ANDROID__)
	{
		uint64_t dt = AndroidProf_Us() - t0;
		ProfPath_Tick(t0);
		gProfPath.calls++;
		gProfPath.comps += comps;
		gProfPath.pathUs += dt;
		if(dt >= 5000)
			AndroidProf_Log("path %s -> %s %u comps, %llu us", win, cur, comps, (unsigned long long)dt);
	}
#endif
}
