// arenas/registry.c — the ARENA manifest + accessors.
//
// The four built-in arenas are the cities' multiplayer maps: identity only
// (unbounded, no authored spawns), so the match runs exactly as it did before
// this registry existed. Every other arena is loaded from an authored file
// under JERICHO/CONFIG/arenas/ (see ARENAS.md) and OVERRIDES a built-in of the
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

#if defined(_WIN32)
#include <windows.h>
#endif

// The authored-file reader/writer lives in arenas/arenafile.c.
int cd2ArenaFileLoad(const char* path, CD2_ARENA_PROFILE* out);

// The registry. Index == arena id.
static CD2_ARENA_PROFILE gArena[CD2_ARENA_MAX_ARENAS];
static int gArenaCount;

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

	return gArenaCount++;
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

		printInfo("[cainescrossfire] arena file '%s': '%s' city=%d mp=%d/%d region=%s spawns=%d pickups=%d\n",
			path, a.internalName, a.city, a.mpLevel, a.mpArena,
			a.region.bounded ? "bounded" : "whole-level", a.spawnCount, a.pickupCount);

		return (id != CD2_ARENA_NONE);
	}
}

// Scan JERICHO/CONFIG/arenas/ for *.cca files. A scan failure is not fatal: the
// built-ins always survive.
static void cd2ArenaScanDir(void)
{
	char dir[512];

	snprintf(dir, sizeof(dir), "%s/CONFIG/arenas", jer_root_dir());

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
