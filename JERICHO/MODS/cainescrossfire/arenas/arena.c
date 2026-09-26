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

#include "driver2.h"
#include "cars.h"
#include "players.h"		/* MainPlayer - the player's car */
#include "jericho.h"
#include "jer_events.h"
#include "jer_hud.h"		/* jer_hud_message - the fallback notice */
#include "cainescrossfire.h"
#include "arenas/profile.h"

#include <string.h>

extern int ratan2(int y, int x);
extern void RebuildCarMatrix(RigidBodyState* st, CAR_DATA* cp);

// ---------------------------------------------------------------------------
// which arena the match is on
// ---------------------------------------------------------------------------
static int gArenaCurrent = CD2_ARENA_NONE;

// Once-per-level state (reset at JER_EVENT_GAME_START).
static int gArenaNotified;
static int gArenaClampLogged[MAX_CARS];	/* first hit against the barrier, per car */

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
int cd2ArenaPlayerSpawn(int* x, int* z, int* heading)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();

	if (a == NULL || a->spawnCount < 1)
		return 0;

	if (x) *x = a->spawns[0].x;
	if (z) *z = a->spawns[0].z;
	if (heading) *heading = a->spawns[0].heading;

	return 1;
}

int cd2ArenaOpponentSpawn(int index, int* x, int* z, int* heading)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int slot = index + 1;		/* spawns[0] is the player */

	if (a == NULL || index < 0 || slot >= a->spawnCount)
		return 0;

	if (x) *x = a->spawns[slot].x;
	if (z) *z = a->spawns[slot].z;
	if (heading) *heading = a->spawns[slot].heading;

	return 1;
}

// ---------------------------------------------------------------------------
// the player's start
// ---------------------------------------------------------------------------
static void cd2ArenaPlacePlayer(void)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();
	int id = player[0].playerCarId;
	CAR_DATA* cp;
	int x, z, heading;

	if (a == NULL || id < 0 || id >= MAX_CARS)
		return;

	if (!cd2ArenaPlayerSpawn(&x, &z, &heading))
		return;

	cp = &car_data[id];

	if (cp->controlType != CONTROL_TYPE_PLAYER)
		return;

	cp->hd.where.t[0] = x;
	cp->hd.where.t[2] = z;
	cp->hd.direction = heading & 0xfff;

	cp->st.n.linearVelocity[0] = 0;
	cp->st.n.linearVelocity[1] = 0;
	cp->st.n.linearVelocity[2] = 0;
	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[1] = 0;
	cp->st.n.angularVelocity[2] = 0;

	RebuildCarMatrix(&cp->st, cp);

	printInfo("[cainescrossfire] arena '%s': player car %d at spawn (%d,%d) heading %d\n",
		a->internalName, id, x, z, heading & 0xfff);
}

// ---------------------------------------------------------------------------
// the barrier
// ---------------------------------------------------------------------------
static void cd2ArenaClampCar(CAR_DATA* cp, const CD2_ARENA_REGION* r)
{
	int x = cp->hd.where.t[0];
	int z = cp->hd.where.t[2];
	int hit = 0;

	if (x < r->x0)      { x = r->x0; if (cp->st.n.linearVelocity[0] < 0) cp->st.n.linearVelocity[0] = 0; hit = 1; }
	else if (x > r->x1) { x = r->x1; if (cp->st.n.linearVelocity[0] > 0) cp->st.n.linearVelocity[0] = 0; hit = 1; }

	if (z < r->z0)      { z = r->z0; if (cp->st.n.linearVelocity[2] < 0) cp->st.n.linearVelocity[2] = 0; hit = 1; }
	else if (z > r->z1) { z = r->z1; if (cp->st.n.linearVelocity[2] > 0) cp->st.n.linearVelocity[2] = 0; hit = 1; }

	if (!hit)
		return;

	cp->hd.where.t[0] = x;
	cp->hd.where.t[2] = z;

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
// hooks
// ---------------------------------------------------------------------------
static int cd2ArenaOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	gArenaNotified = 0;
	memset(gArenaClampLogged, 0, sizeof(gArenaClampLogged));

	/* the level has placed the player by now (GAME_START fires after
	 * State_GameInit's car setup), so this lands on the real car */
	cd2ArenaPlacePlayer();

	return JER_RESULT_CONTINUE;
}

static int cd2ArenaOnFrame(void* ud, void* args)
{
	const CD2_ARENA_PROFILE* a = cd2ArenaCurrent();

	(void)ud;
	(void)args;

	if (a == NULL || MainPlayer.playerCarId < 0 || MainPlayer.playerCarId >= MAX_CARS)
		return JER_RESULT_CONTINUE;

	/* no spawn points authored: say so once, so it is not a silent surprise */
	if (!gArenaNotified && a->spawnCount == 0)
	{
		gArenaNotified = 1;
		jer_hud_message("No spawn points in this arena - using fallback", 180);
		printInfo("[cainescrossfire] arena '%s': no spawn points - using fallback placement\n",
			a->internalName);
	}

	cd2ArenaBarrier();

	return JER_RESULT_CONTINUE;
}

void cd2ArenaRegister(JERICHO_CONTEXT* ctx)
{
	/* late, so cd2OnGameStart (which clears the respawn homes) has already run
	 * and the home it takes on the next frame is the authored spawn */
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2ArenaOnGameStart, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2ArenaOnFrame, NULL, 0);
}
