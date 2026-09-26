// arenas/arena.c — the arena at RUNTIME: spawning cars, the barrier, the notice.
//
// Three jobs, all driven by the arena the select flow chose (cd2ArenaSetCurrent):
//
//   1. SPAWNS. If the arena authored spawn points, the player starts at
//      spawns[0] (applied at JER_EVENT_GAME_START, once the level has placed the
//      car) and the AI opponents start at spawns[1..] (the AI reads them through
//      cd2ArenaOpponentSpawn). With no spawns authored, nothing moves: the
//      engine's start and the module's player-relative opponent placement stand
//      in, and a short on-screen notice says so.
//
//   2. THE BARRIER. A bounded arena keeps every car the module owns inside its
//      region rect: a car at the edge is put back on it and its OUTWARD velocity
//      is cancelled, so the edge reads as a wall. The region is world-space XZ,
//      the same units car_data[].hd.where.t uses. An unbounded arena (the
//      built-ins) touches nothing.
//
//   3. The notice above (jer_hud_message), once per level.
//
//   4. FOLLOWING THE FILE. The arena's .cca is re-read when it changes on disk,
//      so an edit made by the Python editor (tools/arenaedit.py, the same file)
//      lands here without a restart - pseudo-realtime editing between the two.

#include "driver2.h"
#include "cars.h"
#include "players.h"		/* MainPlayer - the player's car */

#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"		/* jer_hud_message - the fallback notice */
#include "cainescrossfire.h"
#include "arenas/profile.h"
#include "main.h"			/* FrameCnt - the CC_YLOG dev probe */

/* The engine's ground height at (x,z) - what a spawn with no authored y needs.
 *
 * MapHeight answers 0 for a cell that is not loaded yet (antfarm notes the same),
 * so 0 means "no answer", NOT "the ground is at 0"; the caller then keeps the
 * height the level gave the car. Without this, every opponent inherited the
 * PLAYER's height, which is only right where the player is standing - put one
 * 100k units away and it spawns in mid-air and drops into the void. */
extern int MapHeight(VECTOR* pos);

int cd2ArenaGroundY(int x, int z)
{
	VECTOR p;
	int h;

	p.vx = x;
	p.vy = 0;
	p.vz = z;

	h = MapHeight(&p);

	return (h != 0) ? h : CD2_ARENA_NO_Y;
}

/* NOTE: a forced UnpackRegion(spawn) + StartSpooling/UpdateSpool here was tried
 * (antfarm's trick for a far teleport) and made things WORSE: the engine keeps
 * only FOUR barrels, so pulling regions in for spawns one at a time evicts the
 * ones just loaded, and MapHeight then answers 0 for spawns that previously had
 * ground. Don't do it. A spawn in a region the engine has not spooled is a real
 * limitation - keep arena spawns inside the area the player is playing in. */

#include "weapons/core/weapon.h"		/* cd2WpnCarGrant / cd2WpnName - a weapon pickup */
#include "weapons/core/weapon_internal.h"	/* cd2WpnLine - the marker */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>		/* getenv - the CC_YLOG dev probe */

#define CD2_PICKUP_RADIUS	320	/* drive-over range (world units) */

// How far outside the rectangle a car may be and still count as "crossing the
// wall" (it is moving, and got there from inside). Beyond this it started out
// there - a spawn outside the region - and pulling it back would be a long
// teleport, quite possibly into the void. Generous enough for a fast overshoot.
#define CD2_ARENA_BARRIER_MARGIN	12000
#define CD2_PICKUP_RESPAWN	900	/* frames a taken pickup stays gone (30s) */
#define CD2_PICKUP_HEIGHT	160	/* marker bar height (y-up) */
#define CD2_PICKUP_Y		0	/* the flat-ground plane the markers stand on */
#define CD2_PICKUP_AMMO_DEFAULT	5
#define CD2_PICKUP_HEALTH_DEFAULT 2500

extern int ratan2(int y, int x);
extern void RebuildCarMatrix(RigidBodyState* st, CAR_DATA* cp);
extern void TempBuildHandlingMatrix(CAR_DATA* cp, int init);

int cd2ArenaFileLoad(const char* path, CD2_ARENA_PROFILE* out);

// ---------------------------------------------------------------------------
// file watch (see job 4 above)
// ---------------------------------------------------------------------------
#define CD2_ARENA_WATCH_FRAMES	20	/* poll every ~0.7s at 30fps */

static unsigned long gArenaHash;
static int gArenaHashSet;
static int gArenaWatchTick;

// FNV-1a over the file's bytes; 0 when it cannot be read (e.g. no file yet).
static unsigned long cd2ArenaFileHash(const char* path)
{
	FILE* fp = fopen(path, "rb");
	unsigned long h = 2166136261u;

	if (fp == NULL)
		return 0;

	for (;;)
	{
		int ch = fgetc(fp);

		if (ch == EOF)
			break;

		h ^= (unsigned char)ch;
		h *= 16777619u;
	}

	fclose(fp);

	return (h != 0) ? h : 1;	/* 0 is reserved for "unreadable" */
}

// Remember the file as it is now, so the watcher does not fire on our own write
// (the in-game editor calls this after it saves).
void cd2ArenaWatchReset(void)
{
	char path[512];
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();

	gArenaHash = 0;
	gArenaHashSet = 0;

	if (a == NULL || !cd2ArenaFilePath(a, path, sizeof(path)))
		return;

	gArenaHash = cd2ArenaFileHash(path);
	gArenaHashSet = 1;
}

static void cd2ArenaWatch(void)
{
	char path[512];
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	unsigned long h;

	if ((++gArenaWatchTick % CD2_ARENA_WATCH_FRAMES) != 0)
		return;

	if (a == NULL || !cd2ArenaFilePath(a, path, sizeof(path)))
		return;

	h = cd2ArenaFileHash(path);

	if (!gArenaHashSet)
	{
		gArenaHash = h;
		gArenaHashSet = 1;
		return;
	}

	if (h == gArenaHash)
		return;

	/* the file changed under us */
	gArenaHash = h;

	if (cd2EditorHasUnsaved())
	{
		printInfo("[cainescrossfire] arena '%s': changed on disk - kept your unsaved in-game edits "
			"(SELECT save or START reload to take the file)\n", a->internalName);
		return;
	}

	{
		CD2_ARENA_PROFILE tmp;

		if (!cd2ArenaFileLoad(path, &tmp))
			return;

		cd2ArenaReplace(a->id, &tmp);

		printInfo("[cainescrossfire] arena: reloaded '%s' from disk (%d spawns, %d pickups)\n",
			tmp.internalName, tmp.spawnCount, tmp.pickupCount);

		if (cd2EditorActive())
			jer_hud_message("arena reloaded from disk", 120);
	}
}

// ---------------------------------------------------------------------------
// which arena the match is on
// ---------------------------------------------------------------------------
static int gArenaCurrent = CD2_ARENA_NONE;

// Once-per-level state (reset at JER_EVENT_GAME_START).
static int gArenaNotified;
static int gArenaPlayerPlaced;
static int gArenaClampLogged[MAX_CARS];	/* first hit against the barrier, per car */
static int gArenaFarLogged[MAX_CARS];	/* first "far outside the region" note, per car */
static int gPickupActive[CD2_ARENA_MAX_PICKUPS];
static int gPickupTimer[CD2_ARENA_MAX_PICKUPS];

const CD2_ARENA_PROFILE* cd2ArenaCurrent(void)
{
	return cd2ArenaDef(gArenaCurrent);
}

void cd2ArenaSetCurrent(int arenaId)
{
	gArenaCurrent = arenaId;
}

// ---------------------------------------------------------------------------
// spawn lookups (used by the AI spawn path)
// ---------------------------------------------------------------------------
int cd2ArenaPlayerSpawn(CD2_ARENA_SPAWN* out)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();

	if (a == NULL || a->spawnCount < 1)
		return 0;

	if (out != NULL)
		*out = a->spawns[0];

	return 1;
}

int cd2ArenaOpponentSpawn(int index, CD2_ARENA_SPAWN* out)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int slot = index + 1;		/* spawns[0] is the player */

	if (a == NULL || index < 0 || slot >= a->spawnCount)
		return 0;

	if (out != NULL)
		*out = a->spawns[slot];

	return 1;
}

// ---------------------------------------------------------------------------
// the player's start
// ---------------------------------------------------------------------------
static void cd2ArenaPlacePlayer(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	CD2_ARENA_SPAWN sp;
	int id = player[0].playerCarId;
	CAR_DATA* cp;

	if (a == NULL || id < 0 || id >= MAX_CARS)
		return;

	if (!cd2ArenaPlayerSpawn(&sp))
		return;

	cp = &car_data[id];

	if (cp->controlType != CONTROL_TYPE_PLAYER)
		return;

	cp->hd.where.t[0] = sp.x;
	cp->hd.where.t[2] = sp.z;

	/* The authored height, when there is one. Otherwise ask the map: a car placed
	 * on a hill or a raised road falls through the world if it inherits a height
	 * that belongs to some other spot. */
	if (sp.y != CD2_ARENA_NO_Y)
		cp->hd.where.t[1] = sp.y;
	else
	{
		int gy = cd2ArenaGroundY(sp.x, sp.z);

		if (gy != CD2_ARENA_NO_Y)
			cp->hd.where.t[1] = gy;
	}

	cp->hd.direction = sp.heading & 0xfff;

	cp->st.n.linearVelocity[0] = 0;
	cp->st.n.linearVelocity[1] = 0;
	cp->st.n.linearVelocity[2] = 0;
	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[1] = 0;
	cp->st.n.angularVelocity[2] = 0;

	/* TempBuildHandlingMatrix(init=1) copies hd.where into the rigid body's
	 * fposition and rebuilds the orientation from hd.direction - the position
	 * and heading the physics actually reads. RebuildCarMatrix alone would walk
	 * the position BACK from fposition, undoing the move. */
	TempBuildHandlingMatrix(cp, 1);

	printInfo("[cainescrossfire] arena '%s': player car %d spawned at (%d,%d,%d) heading %d\n",
		a->internalName, id, cp->hd.where.t[0], cp->hd.where.t[1], cp->hd.where.t[2],
		cp->hd.direction);
}

// ---------------------------------------------------------------------------
// the barrier
// ---------------------------------------------------------------------------
static void cd2ArenaClampCar(CAR_DATA* cp, const CD2_ARENA_REGION* r)
{
	int x = cp->hd.where.t[0];
	int z = cp->hd.where.t[2];
	int hit = 0;

	// A car FAR outside is not "crossing the wall": it is somewhere it should
	// never have started (a spawn outside the region, most likely). Pulling it
	// onto the rectangle would be a long teleport - often straight into the void,
	// which is exactly the "car falls through the world" a bad spawn causes. So
	// leave it where it is and say so once; the load-time warning explains it.
	if (x < r->x0 - CD2_ARENA_BARRIER_MARGIN || x > r->x1 + CD2_ARENA_BARRIER_MARGIN ||
	    z < r->z0 - CD2_ARENA_BARRIER_MARGIN || z > r->z1 + CD2_ARENA_BARRIER_MARGIN)
	{
		if (cp->id >= 0 && cp->id < MAX_CARS && !gArenaFarLogged[cp->id])
		{
			gArenaFarLogged[cp->id] = 1;
			printInfo("[cainescrossfire] arena barrier: car=%d sits far outside the region "
				"(%d,%d) - left alone (a spawn outside the region?)\n", cp->id, x, z);
		}

		return;
	}

	if (x < r->x0)      { x = r->x0; if (cp->st.n.linearVelocity[0] < 0) cp->st.n.linearVelocity[0] = 0; hit = 1; }
	else if (x > r->x1) { x = r->x1; if (cp->st.n.linearVelocity[0] > 0) cp->st.n.linearVelocity[0] = 0; hit = 1; }

	if (z < r->z0)      { z = r->z0; if (cp->st.n.linearVelocity[2] < 0) cp->st.n.linearVelocity[2] = 0; hit = 1; }
	else if (z > r->z1) { z = r->z1; if (cp->st.n.linearVelocity[2] > 0) cp->st.n.linearVelocity[2] = 0; hit = 1; }

	if (!hit)
		return;

	cp->hd.where.t[0] = x;
	cp->hd.where.t[2] = z;

	/* fposition is what the physics reads; move it too (keep the orientation, so
	 * a clamped car keeps its pitch/roll) */
	cp->st.n.fposition[0] = x << 4;
	cp->st.n.fposition[2] = z << 4;

	/* halve the speed into the wall so the clamp reads as a scrape, not a stop */
	if (cp->st.n.linearVelocity[0] && cp->st.n.linearVelocity[2])
	{
		cp->st.n.linearVelocity[0] = cp->st.n.linearVelocity[0] / 2;
		cp->st.n.linearVelocity[2] = cp->st.n.linearVelocity[2] / 2;
	}

	RebuildCarMatrix(&cp->st, cp);

	/* the FIRST hit per car per level, so a test can see the barrier worked */
	if (cp->id >= 0 && cp->id < MAX_CARS && !gArenaClampLogged[cp->id])
	{
		gArenaClampLogged[cp->id] = 1;
		printInfo("[cainescrossfire] arena barrier: car=%d clamped to (%d,%d)\n",
			cp->id, x, z);
	}
}

static void cd2ArenaBarrier(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int i;

	if (a == NULL || !a->region.bounded)
		return;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
			continue;

		/* keep every car the MODULE owns in the ring; civ traffic is free to
		 * wander out (the fight is confined, the world is not) */
		if (!cd2OwnsCar(cp))
			continue;

		cd2ArenaClampCar(cp, &a->region);
	}
}

// ---------------------------------------------------------------------------
// pickups (drive-over weapon crates + repairs)
// ---------------------------------------------------------------------------
static void cd2ArenaDrawPickup(const CD2_ARENA_PICKUP* p)
{
	VECTOR base, tip, e;
	int r, g, b;
	int w = 70;
	int i;
	static const int dir[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };

	if (p->type == CD2_PICKUP_HEALTH) { r = 80; g = 255; b = 120; }
	else                              { r = 255; g = 210; b = 120; }

	base.vx = p->x;
	base.vy = CD2_PICKUP_Y;
	base.vz = p->z;

	tip = base;
	tip.vy = base.vy + CD2_PICKUP_HEIGHT;

	/* a bar standing on the ground ... */
	cd2WpnLine(&base, &tip, r, g, b);

	/* ... topped by a small diamond so it reads as a pickup, not a post */
	for (i = 0; i < 4; i++)
	{
		int j = (i + 1) & 3;
		VECTOR a2, b2;

		a2.vx = tip.vx + dir[i][0] * w; a2.vy = tip.vy; a2.vz = tip.vz + dir[i][1] * w;
		b2.vx = tip.vx + dir[j][0] * w; b2.vy = tip.vy; b2.vz = tip.vz + dir[j][1] * w;
		cd2WpnLine(&a2, &b2, r, g, b);
	}

	/* a foot ring so it is findable from a distance */
	for (i = 0; i < 4; i++)
	{
		int j = (i + 1) & 3;
		e.vx = base.vx + dir[i][0] * (w + 20); e.vy = base.vy; e.vz = base.vz + dir[i][1] * (w + 20);
		VECTOR f;
		f.vx = base.vx + dir[j][0] * (w + 20); f.vy = base.vy; f.vz = base.vz + dir[j][1] * (w + 20);
		cd2WpnLine(&e, &f, r, g, b);
	}
}

static void cd2ArenaPickupTake(CAR_DATA* cp, const CD2_ARENA_PICKUP* p)
{
	int isPlayer = (cp->controlType == CONTROL_TYPE_PLAYER);

	if (p->type == CD2_PICKUP_WEAPON)
	{
		int ammo = (p->amount > 0) ? p->amount : CD2_PICKUP_AMMO_DEFAULT;

		cd2WpnCarGrant(cp, p->weapon, ammo);

		printInfo("[cainescrossfire] pickup: car=%d got %s x%d\n",
			cp->id, cd2WpnName(p->weapon), ammo);

		if (isPlayer)
		{
			char msg[64];

			snprintf(msg, sizeof(msg), "Picked up %s x%d", cd2WpnName(p->weapon), ammo);
			jer_hud_message(msg, 120);
		}
	}
	else
	{
		int amount = (p->amount > 0) ? p->amount : CD2_PICKUP_HEALTH_DEFAULT;
		int before = (int)cp->totalDamage;

		if (cp->totalDamage > (unsigned int)amount)
			cp->totalDamage -= amount;
		else
			cp->totalDamage = 0;

		printInfo("[cainescrossfire] pickup: car=%d repaired %d (totalDamage %d -> %d)\n",
			cp->id, amount, before, (int)cp->totalDamage);

		if (isPlayer)
		{
			char msg[64];

			snprintf(msg, sizeof(msg), "Repaired +%d", amount);
			jer_hud_message(msg, 120);
		}
	}
}

static void cd2ArenaPickups(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	long long r2 = (long long)CD2_PICKUP_RADIUS * CD2_PICKUP_RADIUS;
	int i, c;

	if (a == NULL)
		return;

	for (i = 0; i < a->pickupCount && i < CD2_ARENA_MAX_PICKUPS; i++)
	{
		const CD2_ARENA_PICKUP* p = &a->pickups[i];

		if (!gPickupActive[i])
		{
			if (gPickupTimer[i] > 0 && --gPickupTimer[i] <= 0)
				gPickupActive[i] = 1;

			continue;
		}

		cd2ArenaDrawPickup(p);

		for (c = 0; c < MAX_CARS; c++)
		{
			CAR_DATA* cp = &car_data[c];
			long long dx, dz, d2;

			if (cp->controlType == CONTROL_TYPE_NONE || cp->ap.carCos == NULL)
				continue;

			/* only a car in the match collects (civ traffic drives over freely) */
			if (!cd2OwnsCar(cp))
				continue;

			dx = (long long)cp->hd.where.t[0] - p->x;
			dz = (long long)cp->hd.where.t[2] - p->z;
			d2 = dx * dx + dz * dz;

			if (d2 > r2)
				continue;

			cd2ArenaPickupTake(cp, p);
			gPickupActive[i] = 0;
			gPickupTimer[i] = CD2_PICKUP_RESPAWN;
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------
// Warn about a spawn that sits outside the region: the barrier will NOT pull it
// in (see cd2ArenaClampCar), so that car may drop through the world when the
// match starts. Said once at GAME_START, in the log and on screen, because it is
// the author's to fix and otherwise looks like a random crash into the void.
static void cd2ArenaWarnSpawns(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int i, bad = 0;

	if (a == NULL || !a->region.bounded)
		return;

	for (i = 0; i < a->spawnCount && i < CD2_ARENA_MAX_SPAWNS; i++)
	{
		const CD2_ARENA_SPAWN* sp = &a->spawns[i];

		if (sp->x < a->region.x0 || sp->x > a->region.x1 ||
		    sp->z < a->region.z0 || sp->z > a->region.z1)
		{
			printInfo("[cainescrossfire] arena '%s': spawn %d (%d,%d) is OUTSIDE the region "
				"(%d,%d,%d,%d) - it will not be pulled in, so it may fall into the void\n",
				a->internalName, i, sp->x, sp->z,
				a->region.x0, a->region.z0, a->region.x1, a->region.z1);
			bad++;
		}
	}

	if (bad > 0)
		jer_hud_message("spawn outside the arena region - fix it in the arena editor", 240);
}

/* Is there actually ground where each spawn is?
 *
 * This is the question "did I mess the spawn up, or is it on the world?" - asked
 * of the engine instead of guessed. MapHeight answers 0 for a cell that is not
 * loaded, and a spawn off the edge of the map is never loaded either, so both
 * faults show up the same way: no ground, and a car dropped there falls into the
 * void. Runs on the first FRAME (not GAME_START) so the cells have been spooled. */
static void cd2ArenaReportGround(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int i, void_spawns = 0;

	if (a == NULL || a->spawnCount == 0)
		return;

	for (i = 0; i < a->spawnCount && i < CD2_ARENA_MAX_SPAWNS; i++)
	{
		const CD2_ARENA_SPAWN* sp = &a->spawns[i];
		int gy = cd2ArenaGroundY(sp->x, sp->z);

		if (gy == CD2_ARENA_NO_Y)
		{
			printInfo("[cainescrossfire] arena '%s': spawn %d (%d,%d) has NO GROUND there - "
				"off the map, or not loaded. A car dropped there falls into the void.\n",
				a->internalName, i, sp->x, sp->z);
			void_spawns++;
		}
		else
		{
			printInfo("[cainescrossfire] arena '%s': spawn %d (%d,%d) is on the world, ground y=%d%s\n",
				a->internalName, i, sp->x, sp->z, gy,
				(sp->y != CD2_ARENA_NO_Y) ? " (the authored height is what is used)" : "");
		}
	}

	if (void_spawns > 0)
		jer_hud_message("a spawn has no ground under it - you would fall into the void", 240);
}

static int cd2ArenaOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	gArenaNotified = 0;
	memset(gArenaClampLogged, 0, sizeof(gArenaClampLogged));
	memset(gArenaFarLogged, 0, sizeof(gArenaFarLogged));

	/* every pickup starts present */
	{
		const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
		int i, n = (a != NULL) ? a->pickupCount : 0;

		if (n > CD2_ARENA_MAX_PICKUPS)
			n = CD2_ARENA_MAX_PICKUPS;

		for (i = 0; i < CD2_ARENA_MAX_PICKUPS; i++)
		{
			gPickupActive[i] = (i < n) ? 1 : 0;
			gPickupTimer[i] = 0;
		}

		if (a != NULL && n > 0)
			printInfo("[cainescrossfire] arena '%s': %d pickup(s) armed\n", a->internalName, n);
	}

	/* The engine places the player AFTER GAME_START (its own start position is
	 * written later in the launch), so the player's authored spawn is applied on
	 * the first FRAME instead - see cd2ArenaOnFrame. */
	gArenaPlayerPlaced = 0;

	/* a spawn outside the region will not be pulled in - say so up front */
	cd2ArenaWarnSpawns();

	/* take the file as it is now, so the watcher only fires on a LATER change */
	cd2ArenaWatchReset();

	return JER_RESULT_CONTINUE;
}

static int cd2ArenaOnFrame(void* ud, void* args)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	CAR_DATA* pcp = NULL;

	(void)ud;
	(void)args;

	if (a == NULL || !cd2WpnPlayerCar(&pcp))
		return JER_RESULT_CONTINUE;

	/* the player's authored spawn, once - on the first frame the real car exists
	 * (the engine writes its own start position after GAME_START) */
	if (!gArenaPlayerPlaced)
	{
		gArenaPlayerPlaced = 1;
		cd2ArenaPlacePlayer();

		/* now that the cells are spooled, say whether each spawn has ground */
		cd2ArenaReportGround();
	}

	/* no spawn points authored: say so once, so it is not a silent surprise */
	if (!gArenaNotified && a->spawnCount == 0)
	{
		gArenaNotified = 1;
		jer_hud_message("No spawn points in this arena - using fallback", 180);
		printInfo("[cainescrossfire] arena '%s': no spawn points - using fallback placement\n",
			a->internalName);
	}

	cd2ArenaBarrier();
	cd2ArenaPickups();
	cd2ArenaWatch();

	/* TEMP dev probe: CC_YLOG=1 prints the player's height for the first 90
	 * frames, to see whether a spawn settles on the ground or falls. */
	{
		static int ylog = -1;

		if (ylog < 0)
		{
			const char* e = getenv("CC_YLOG");

			ylog = (e != NULL && e[0] == '1');
		}

		{
			CAR_DATA* ycp = &car_data[player[0].playerCarId];

			if (ylog && FrameCnt < 90 && ycp->controlType == CONTROL_TYPE_PLAYER)
				printInfo("[cd2ylog] frame=%d y=%d vy=%d\n", FrameCnt,
					ycp->hd.where.t[1], ycp->st.n.linearVelocity[1]);
		}
	}

	return JER_RESULT_CONTINUE;
}

void cd2ArenaRegister(JERICHO_CONTEXT* ctx)
{
	/* late, so cd2OnGameStart (which clears the respawn homes) has already run
	 * and the home it takes on the next frame is the authored spawn */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2ArenaOnGameStart, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2ArenaOnFrame, NULL, 0);
}
