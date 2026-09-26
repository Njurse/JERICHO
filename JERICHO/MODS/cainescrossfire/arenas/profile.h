// arenas/profile.h — Combat D2 ARENA registry: named, bounded play areas.
//
// An ARENA is a bounded place a match happens on: a city (a LevelNames index),
// which mission layout to load, a REGION (the barrier that keeps the fight in),
// and a list of SPAWN POINTS (each a position + heading). Where a VEHICLE
// PROFILE (profiles/) says *what car*, an ARENA PROFILE says *where*.
//
// The registry is the same shape as the vehicle one (profiles/registry.c): a
// manifest of rows indexed by an id. The four built-ins are the cities'
// multiplayer maps (unbounded, no authored spawns - the match falls back to the
// engine's start and the module's player-relative opponent placement). Every
// OTHER arena comes from an authored DATA FILE under JERICHO/CONFIG/arenas/
// (see ARENAS.md): the same "rows are the manifest, data is the content" split
// the profiles use, so a custom arena - including a cordoned-off corner of a
// full city map - needs no rebuild, only a file the editor writes.

#ifndef CD2_ARENA_PROFILE_H
#define CD2_ARENA_PROFILE_H

#include "driver2.h"
#include "cainescrossfire.h"

// Built-in arena ids (index into the manifest in arenas/registry.c). Custom
// arenas loaded from files take ids after these, in load order.
enum
{
	CD2_ARENA_CHICAGO = 0,
	CD2_ARENA_HAVANA,
	CD2_ARENA_VEGAS,
	CD2_ARENA_RIO,
	CD2_ARENA_BUILTIN_COUNT,
	CD2_ARENA_NONE = -1
};

// A rectangular play area in WORLD units (XZ - the same units
// car_data[].hd.where.t uses). `bounded` = 0 means "the whole level": no
// barrier, the fallback for a city with no authored region.
typedef struct CD2_ARENA_REGION
{
	int bounded;		// 0 = whole level, 1 = keep every car inside the rect
	int x0, z0, x1, z1;	// world units; x0<x1 and z0<z1 after load
} CD2_ARENA_REGION;

// One spawn point: world position + heading (PSX heading units, 0..4095).
typedef struct CD2_ARENA_SPAWN
{
	int x, z;
	int heading;
} CD2_ARENA_SPAWN;

#define CD2_ARENA_MAX_SPAWNS	16
#define CD2_ARENA_MAX_ARENAS	32
#define CD2_ARENA_NAME_LEN	32
#define CD2_ARENA_DISPLAY_LEN	48

// A drive-over pickup: a weapon crate or a repair. Placed like a spawn (a world
// XZ), collected by a module-owned car that drives within range, then gone for a
// while (CD2_PICKUP_RESPAWN). `amount` is rounds for a weapon, damage units
// removed from the car's totalDamage for a repair.
enum
{
	CD2_PICKUP_WEAPON = 0,
	CD2_PICKUP_HEALTH
};

typedef struct CD2_ARENA_PICKUP
{
	int type;	// CD2_PICKUP_*
	int weapon;	// CD2_WID_* (a weapon pickup), else -1
	int amount;	// rounds, or damage units
	int x, z;
} CD2_ARENA_PICKUP;

#define CD2_ARENA_MAX_PICKUPS	32

// The arena row. spawns[0] is the PLAYER's start; spawns[1..] fill the
// opponents in order. A spawnCount of 0 means "none authored" - the caller
// falls back (and says so on screen).
typedef struct CD2_ARENA_PROFILE
{
	int id;					// CD2_ARENA_* or a runtime index
	char internalName[CD2_ARENA_NAME_LEN];	// "chicago" (code-facing / file key)
	char displayName[CD2_ARENA_DISPLAY_LEN];// "Chicago" (on-screen)

	int city;		// LevelNames index (0..3) - which city the arena is on
	int mpLevel;		// 1 = load the city's small multiplayer map, 0 = the full city
	int mpArena;		// which of the city's two mp layouts (only when mpLevel)

	CD2_ARENA_REGION region;		// the barrier (unbounded by default)
	int spawnCount;				// authored spawn points (0 = fall back)
	CD2_ARENA_SPAWN spawns[CD2_ARENA_MAX_SPAWNS];

	int pickupCount;			// drive-over weapon/health pickups
	CD2_ARENA_PICKUP pickups[CD2_ARENA_MAX_PICKUPS];
} CD2_ARENA_PROFILE;

// ---------------------------------------------------------------------------
// The registry (arenas/registry.c)
// ---------------------------------------------------------------------------
// How many arenas are registered (built-ins + anything loaded from a file).
int cd2ArenaCount(void);

// The arena row for an id, or NULL when the id is not in range.
const CD2_ARENA_PROFILE* cd2ArenaDef(int arenaId);

// The first arena whose internalName or displayName matches `name` (exact,
// case-sensitive), or NULL. Used by the loader to let a file OVERRIDE a
// built-in of the same internal name.
const CD2_ARENA_PROFILE* cd2ArenaFindByName(const char* name);

// Register (or replace, by internalName) an arena. Returns its id, or
// CD2_ARENA_NONE when the table is full. Used by the loader; the editor path
// uses it too.
int cd2ArenaRegister(const CD2_ARENA_PROFILE* arena);

// Overwrite an existing arena in place (the in-game editor). Returns 1 on success.
int cd2ArenaReplace(int arenaId, const CD2_ARENA_PROFILE* arena);

// The path an arena saves to: CONFIG/arenas/<internalName>.cca. Returns 1 and
// fills `out` (cap bytes), or 0 on a bad argument.
int cd2ArenaFilePath(const CD2_ARENA_PROFILE* arena, char* out, int cap);

// Build the registry: the four built-ins, then every authored file under
// JERICHO/CONFIG/arenas/. Safe to call more than once (it rebuilds). Call from
// the module entry, before the select flow builds its menus.
void cd2ArenaLoadAll(void);

// One line per arena: the evidence a headless run is read for.
void cd2ArenaDump(void);

// ---------------------------------------------------------------------------
// The match's current arena (arenas/arena.c)
// ---------------------------------------------------------------------------
// The arena the running match is on (set by the select flow), or NULL.
const CD2_ARENA_PROFILE* cd2ArenaCurrent(void);
void cd2ArenaSetCurrent(int arenaId);

// Register the arena hooks (spawn placement, the region barrier, the
// no-spawns fallback notice). Called once from the module entry.
struct JERICHO_CONTEXT;
void cd2ArenaRegister(struct JERICHO_CONTEXT* ctx);

// The CURRENT arena's spawn points, if it has any. cd2ArenaPlayerSpawn reads
// spawns[0]; cd2ArenaOpponentSpawn reads spawns[1 + index]. Each returns 1 and
// fills the three out-params when the spawn exists, else 0 (so the caller falls
// back to its own placement).
int cd2ArenaPlayerSpawn(int* x, int* z, int* heading);
int cd2ArenaOpponentSpawn(int index, int* x, int* z, int* heading);

#endif /* CD2_ARENA_PROFILE_H */
