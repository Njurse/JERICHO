// arenas/registry.c — the ARENA manifest + accessors.
//
// The four built-in arenas are the cities' multiplayer maps: identity only
// (unbounded, no authored spawns), so the match runs exactly as it did before
// this registry existed. Every other arena is loaded from an authored file
// under JERICHO/MODS/cainescrossfire/arenas/ (see ARENAS.md) and OVERRIDES a built-in of the
// same internalName, or is appended as a new arena. That is the same shape as
// the vehicle registry (profiles/registry.c): the built-ins are the manifest,
// the files are the content.

#include "driver2.h"
#include "jericho.h"
#include "cainescrossfire.h"
#include "profiles/profile.h"	/* CD2_VEH_CITY_* - the city ids the built-ins use */
#include "arenas/profile.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>	/* stat - is a candidate arena folder actually there? */
#include <stdlib.h>	/* _fullpath - freeze the arena folder to an absolute path */

#if defined(_WIN32)
#include <windows.h>
#endif

// The authored-file reader/writer lives in arenas/arenafile.c.
int cd2ArenaFileLoad(const char* path, CD2_ARENA_PROFILE* out);

// The registry. Index == arena id.
static CD2_ARENA_PROFILE gArena[CD2_ARENA_MAX_ARENAS];
static int gArenaCount;

// The FILE each arena came from (a built-in has none). Remembered rather than
// derived from the arena's name, so an arena whose `arena:` name differs from
// its file name still saves/reloads where it was loaded from.
static char gArenaPath[CD2_ARENA_MAX_ARENAS][512];

// Where the arena files live: the module's OWN folder (JERICHO/MODS/
// cainescrossfire/arenas), so an arena ships with Caine's Crossfire rather than
// in a separate CONFIG tree outside the mod. Read AND written there, which is
// what makes the two editors - and the launcher - agree.
#define CD2_ARENA_DIR	"MODS/cainescrossfire/arenas"

static int cd2DirExists(const char* path)
{
	struct stat st;

	return (stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0);
}

// The arena folder. A dev build runs from <repo>/src_rebuild/bin/<config>/JERICHO,
// so the repo's own mod folder sits four levels up: prefer THAT, so the game, the
// Python editor (which edits the repo copy) and the launcher all touch ONE file.
// A shipped mod has no repo tree and uses the MODS mirror next to the exe.
//
// The result is frozen to an ABSOLUTE path at boot: the game changes its working
// directory while loading a level, so a relative path stops resolving later -
// which is exactly when the file watcher and the editor's save run.
static const char* cd2ArenaDir(void)
{
	static char dir[512];
	static int done;

	if (!done)
	{
		char dev[512];
		char raw[512];

		done = 1;

		snprintf(dev, sizeof(dev), "%s/../../../../JERICHO/" CD2_ARENA_DIR, jer_root_dir());

		if (cd2DirExists(dev))
			snprintf(raw, sizeof(raw), "%s", dev);
		else
			snprintf(raw, sizeof(raw), "%s/" CD2_ARENA_DIR, jer_root_dir());

#if defined(_WIN32)
		if (_fullpath(dir, raw, sizeof(dir)) == NULL)
			snprintf(dir, sizeof(dir), "%s", raw);
#else
		if (realpath(raw, dir) == NULL)
			snprintf(dir, sizeof(dir), "%s", raw);
#endif
	}

	return dir;
}

// --- built-ins -------------------------------------------------------------

typedef struct CD2_ARENA_BUILTIN
{
	const char* internalName;
	const char* displayName;
	int city;
} CD2_ARENA_BUILTIN;

static const CD2_ARENA_BUILTIN gBuiltins[CD2_ARENA_BUILTIN_COUNT] =
{
	{ "chicago", "Chicago",   CD2_VEH_CITY_CHICAGO },	/* CD2_ARENA_CHICAGO */
	{ "havana",  "Havana",    CD2_VEH_CITY_HAVANA  },	/* CD2_ARENA_HAVANA  */
	{ "vegas",   "Las Vegas", CD2_VEH_CITY_VEGAS   },	/* CD2_ARENA_VEGAS   */
	{ "rio",     "Rio",       CD2_VEH_CITY_RIO     }	/* CD2_ARENA_RIO     */
};

static void cd2ArenaSetNames(CD2_ARENA_PROFILE* a, const char* internal, const char* display)
{
	strncpy(a->internalName, (internal != NULL) ? internal : "", CD2_ARENA_NAME_LEN - 1);
	a->internalName[CD2_ARENA_NAME_LEN - 1] = 0;

	strncpy(a->displayName, (display != NULL) ? display : "", CD2_ARENA_DISPLAY_LEN - 1);
	a->displayName[CD2_ARENA_DISPLAY_LEN - 1] = 0;
}

// --- accessors -------------------------------------------------------------

int cd2ArenaCount(void)
{
	return gArenaCount;
}

const CD2_ARENA_PROFILE* cd2ArenaDef(int arenaId)
{
	if (arenaId < 0 || arenaId >= gArenaCount)
		return NULL;

	return &gArena[arenaId];
}

const CD2_ARENA_PROFILE* cd2ArenaFindByName(const char* name)
{
	int i;

	if (name == NULL || name[0] == 0)
		return NULL;

	for (i = 0; i < gArenaCount; i++)
	{
		if (strcmp(gArena[i].internalName, name) == 0 ||
		    strcmp(gArena[i].displayName, name) == 0)
			return &gArena[i];
	}

	return NULL;
}

int cd2ArenaRegister(const CD2_ARENA_PROFILE* arena)
{
	if (arena == NULL)
		return CD2_ARENA_NONE;

	if (gArenaCount >= CD2_ARENA_MAX_ARENAS)
	{
		printInfo("[cainescrossfire] arenas: table full (%d), dropping '%s'\n",
			CD2_ARENA_MAX_ARENAS, arena->internalName);
		return CD2_ARENA_NONE;
	}

	gArena[gArenaCount] = *arena;
	gArena[gArenaCount].id = gArenaCount;
	gArenaPath[gArenaCount][0] = 0;

	return gArenaCount++;
}

// Overwrite an existing arena in place (the in-game editor saves into it, so the
// region barrier and the next spawn pick up the edit without a reload).
int cd2ArenaReplace(int arenaId, const CD2_ARENA_PROFILE* arena)
{
	if (arena == NULL || arenaId < 0 || arenaId >= gArenaCount)
		return 0;

	gArena[arenaId] = *arena;
	gArena[arenaId].id = arenaId;

	return 1;
}

// The path an arena is (or would be) saved to: MODS/cainescrossfire/arenas/<internalName>.cca.
int cd2ArenaFilePath(const CD2_ARENA_PROFILE* arena, char* out, int cap)
{
	if (arena == NULL || out == NULL || cap <= 0)
		return 0;

	if (arena->id >= 0 && arena->id < gArenaCount && gArenaPath[arena->id][0] != 0)
		snprintf(out, cap, "%s", gArenaPath[arena->id]);
	else
		snprintf(out, cap, "%s/%s.cca", cd2ArenaDir(), arena->internalName);

	return 1;
}

// --- load ------------------------------------------------------------------

// Register (or replace, by internalName) an arena. A file's arena that names a
// built-in REPLACES it in place, so 'chicago' stays id 0 and the menu order is
// stable however many files there are.
static int cd2ArenaRegisterOrReplace(const CD2_ARENA_PROFILE* arena)
{
	int i;

	for (i = 0; i < gArenaCount; i++)
	{
		if (strcmp(gArena[i].internalName, arena->internalName) == 0)
		{
			int id = gArena[i].id;

			gArena[i] = *arena;
			gArena[i].id = id;
			return id;
		}
	}

	return cd2ArenaRegister(arena);
}

static void cd2ArenaAddBuiltins(void)
{
	int i;

	for (i = 0; i < CD2_ARENA_BUILTIN_COUNT; i++)
	{
		CD2_ARENA_PROFILE a;

		memset(&a, 0, sizeof(a));
		a.city = gBuiltins[i].city;
		a.mpLevel = 1;		/* the city's small multiplayer map */
		a.mpArena = 0;
		a.region.bounded = 0;	/* whole level: no barrier */
		a.spawnCount = 0;	/* no authored spawns: the match falls back */

		cd2ArenaSetNames(&a, gBuiltins[i].internalName, gBuiltins[i].displayName);

		cd2ArenaRegisterOrReplace(&a);
	}
}

// Load one authored file and register its arena. Returns 1 when it named an
// arena that was registered.
static int cd2ArenaLoadFile(const char* path)
{
	CD2_ARENA_PROFILE a;

	memset(&a, 0, sizeof(a));

	if (!cd2ArenaFileLoad(path, &a))
	{
		printInfo("[cainescrossfire] arena file '%s': failed to parse\n", path);
		return 0;
	}

	if (a.internalName[0] == 0)
	{
		printInfo("[cainescrossfire] arena file '%s': no arena name\n", path);
		return 0;
	}

	{
		int id = cd2ArenaRegisterOrReplace(&a);

		if (id >= 0 && id < CD2_ARENA_MAX_ARENAS)
			snprintf(gArenaPath[id], sizeof(gArenaPath[id]), "%s", path);

		printInfo("[cainescrossfire] arena file '%s': '%s' city=%d mp=%d/%d region=%s spawns=%d pickups=%d\n",
			path, a.internalName, a.city, a.mpLevel, a.mpArena,
			a.region.bounded ? "bounded" : "whole-level", a.spawnCount, a.pickupCount);

		return (id != CD2_ARENA_NONE);
	}
}

// Scan JERICHO/MODS/cainescrossfire/arenas/ for *.cca files. A scan failure is not fatal: the
// built-ins always survive.
static void cd2ArenaScanDir(void)
{
	const char* dir = cd2ArenaDir();

#if defined(_WIN32)
	{
		char pattern[560];
		WIN32_FIND_DATAA fd;
		HANDLE h;

		snprintf(pattern, sizeof(pattern), "%s/*.cca", dir);

		h = FindFirstFileA(pattern, &fd);

		if (h == INVALID_HANDLE_VALUE)
			return;

		do
		{
			char path[560];

			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
			cd2ArenaLoadFile(path);
		}
		while (FindNextFileA(h, &fd));

		FindClose(h);
	}
#else
	(void)dir;	/* only the Windows build scans the folder today */
#endif
}

void cd2ArenaLoadAll(void)
{
	gArenaCount = 0;

	cd2ArenaAddBuiltins();
	cd2ArenaScanDir();

	printInfo("[cainescrossfire] arenas: dir %s\n", cd2ArenaDir());
	printInfo("[cainescrossfire] arenas: %d registered (%d built-in)\n",
		gArenaCount, CD2_ARENA_BUILTIN_COUNT);
}

// --- evidence --------------------------------------------------------------

void cd2ArenaDump(void)
{
	int i;

	printInfo("[cainescrossfire] arenas: %d registered\n", gArenaCount);

	for (i = 0; i < gArenaCount; i++)
	{
		const CD2_ARENA_PROFILE* a = &gArena[i];

		printInfo("[cainescrossfire]   [%d] %s (%s) city=%d mp=%d/%d region=%d,%d,%d,%d spawns=%d\n",
			i, a->internalName, a->displayName, a->city, a->mpLevel, a->mpArena,
			a->region.x0, a->region.z0, a->region.x1, a->region.z1, a->spawnCount);
	}
}
