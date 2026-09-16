// weapons/core/crew.c — Combat D2 MOUNTED CREW: lean request state + ped lifecycle.
//
// Two halves, one system:
//
//   1. The REQUEST (which window a crew member should be leaning out of). Every
//      weapon fire — the player's and the AI's — comes through cd2WpnTryFire,
//      which calls cd2CrewNotifyFire with that weapon's lean flags. Flags OR
//      together and each set side gets its hold refreshed, so a side that any
//      leaning weapon wants out stays out even while another firing weapon (the
//      base MG, leanOut 0) wants nobody.
//
//   2. The PED lifecycle, driven off that request. While a side's hold is live
//      a TANNER_MODEL ped (model 0) is spawned beside the car's door, plays the
//      engine's PED_ACTION_GETOUTCAR (get out) animation to its final frame and
//      HOLDS there, re-placed from the car's transform every frame so it rides
//      the car. When the hold lapses it plays PED_ACTION_GETINCAR (get back in)
//      and is destroyed as it disappears. We drive the frames ourselves rather
//      than through the engine's PedGetInCar/PedGetOutCar (those hand the car
//      and the peds back to the PLAYER and would hijack the player's vehicle).
//
// Both crew members use Tanner's model (model 0) — the only ped model that is
// always loaded and that renders through the skeleton path (customers use the
// sprite path, which cannot play the get-out pose and gets recycled by the
// ambient system).

#include "driver2.h"
#include "combatd2.h"
#include "cars.h"
#include "camera.h"
#include "dr2roads.h"		/* MapHeight */
#include "pedest.h"		/* LPPEDESTRIAN, pUsedPeds, DestroyPedestrian */
#include "jericho.h"
#include "jer_events.h"
#include "jer_npc.h"		/* jer_npc_* ped scaffolding */
#include "ai/ai.h"		/* cd2AiIsOpponent */
#include "crew.h"

// How long a side stays out after its last leaning shot, in frames (~0.8s at
// 30fps). The hold is RE-ARMED on every shot, so it only needs to bridge the
// gap between shots: shorter than any leaning weapon's refire (the shotgun is
// ~2.2s) yet long enough that a held trigger keeps the ped out continuously.
#define CD2_CREW_HOLD_FRAMES	24

// The get-out pose is held at the last "climbing out" frame; the engine's own
// transition would fire at 15. The get-in animation plays 0..14 and the ped is
// destroyed as it reaches 15 (matching PedGetInCar's `frame1 < 0xf`).
#define CD2_CREW_GETOUT_LAST	14
#define CD2_CREW_GETIN_LAST	15

// No crew is spawned for a car further than this from the camera (world units):
// it would never be drawn, and the ped pool is shared with the ambient civs.
// Generous, because the chase camera sits ~1000 units behind the player's car.
#define CD2_CREW_MAX_DIST	2000

// Crew ped states.
enum { CD2_CREW_IN = 0, CD2_CREW_OUT, CD2_CREW_INANIM };

// Side indices (0 = driver / left, 1 = gunner / right). The matching lean flag
// is (1 << index) == CD2_CREW_DRIVER/GUNNER.
enum { CD2_CREW_SIDE_DRIVER = 0, CD2_CREW_SIDE_GUNNER = 1 };

typedef struct CD2_CREW_CAR
{
	int hold[2];		// frames the side stays out (0 = in)
	JerNpc* ped[2];		// the crew ped (NULL = in the car)
	int state[2];		// CD2_CREW_IN / OUT / INANIM
	int frame[2];		// animation frame within the current phase
} CD2_CREW_CAR;

static CD2_CREW_CAR gCrew[MAX_CARS];

// ---------------------------------------------------------------------------
// request
// ---------------------------------------------------------------------------

void cd2CrewNotifyFire(const CAR_DATA* cp, int leanMask)
{
	CAR_DATA* c = (CAR_DATA*)cp;

	if (c == NULL || c->id < 0 || c->id >= MAX_CARS)
		return;

	if (leanMask & CD2_CREW_DRIVER)
		gCrew[c->id].hold[CD2_CREW_SIDE_DRIVER] = CD2_CREW_HOLD_FRAMES;

	if (leanMask & CD2_CREW_GUNNER)
		gCrew[c->id].hold[CD2_CREW_SIDE_GUNNER] = CD2_CREW_HOLD_FRAMES;

	// Observability: a leaning shot fired (leanMask 0 = the MG / a non-leaning
	// weapon, deliberately silent). This is what a headless run greps to prove
	// the flags reach the crew state for the player AND for AI cars.
	if (leanMask != 0 && gCd2Cfg.debugLog)
		printInfo("[combatd2] crew: car=%d fired lean=0x%X -> driver=%d gunner=%d\n",
			c->id, leanMask,
			gCrew[c->id].hold[CD2_CREW_SIDE_DRIVER],
			gCrew[c->id].hold[CD2_CREW_SIDE_GUNNER]);
}

int cd2CrewSideOut(const CAR_DATA* cp, int side)
{
	const CAR_DATA* c = cp;
	int i;

	if (c == NULL || c->id < 0 || c->id >= MAX_CARS)
		return 0;

	i = (side == CD2_CREW_GUNNER) ? CD2_CREW_SIDE_GUNNER : CD2_CREW_SIDE_DRIVER;

	return gCrew[c->id].hold[i] > 0;
}

int cd2CrewPedCount(void)
{
	int i, side;
	int n = 0;

	for (i = 0; i < MAX_CARS; i++)
		for (side = 0; side < 2; side++)
			if (gCrew[i].ped[side] != NULL)
				n++;

	return n;
}

int cd2CrewPedPos(const CAR_DATA* cp, int side, int out[3])
{
	const CAR_DATA* c = cp;
	LPPEDESTRIAN pPed;
	int i;

	if (c == NULL || c->id < 0 || c->id >= MAX_CARS)
		return 0;

	i = (side == CD2_CREW_GUNNER) ? CD2_CREW_SIDE_GUNNER : CD2_CREW_SIDE_DRIVER;

	pPed = (LPPEDESTRIAN)gCrew[c->id].ped[i];

	if (pPed == NULL)
		return 0;

	out[0] = pPed->position.vx;
	out[1] = pPed->position.vy;
	out[2] = pPed->position.vz;

	return 1;
}

// ---------------------------------------------------------------------------
// ped lifecycle
// ---------------------------------------------------------------------------

// A car the module drives: the player's car, or an AI opponent. Traffic and
// empty slots never carry crew.
static int cd2CrewOwned(const CAR_DATA* cp)
{
	return cp->controlType == CONTROL_TYPE_PLAYER || cd2AiIsOpponent(cp);
}

// Is the car close enough to the camera to bother with a crew ped?
static int cd2CrewNear(const CAR_DATA* cp)
{
	long long dx = (long long)cp->hd.where.t[0] - camera_position.vx;
	long long dz = (long long)cp->hd.where.t[2] - camera_position.vz;

	return (dx * dx + dz * dz) <= (long long)CD2_CREW_MAX_DIST * CD2_CREW_MAX_DIST;
}

// A crew ped can be destroyed from under us (level reset, a cutscene). pUsedPeds
// is the authority on what exists, so trust it rather than a stale pointer.
static int cd2CrewPedAlive(LPPEDESTRIAN p)
{
	LPPEDESTRIAN q = pUsedPeds;

	while (q != NULL)
	{
		if (q == p)
			return 1;

		q = q->pNext;
	}

	return 0;
}

// Hang the ped on the car's door: the side of the body just outside the panel,
// a touch toward the cabin's rear, facing outward, grounded against the map at
// its own x/z. Driven by the same box/matrix math the weapon muzzles use.
static void cd2CrewPlace(LPPEDESTRIAN pPed, const CAR_DATA* cp, int i)
{
	const MATRIX* w = &cp->hd.where;
	const SVECTOR* cb = &cp->ap.carCos->colBox;
	int s = (i == CD2_CREW_SIDE_DRIVER) ? -1 : 1;	// left = driver
	int lat = (cb->vx * 108) / 100;			// just outside the body side
	int fwd = -(cb->vz / 8);			// slightly behind the door line
	int x, z, yaw;
	VECTOR g;

	x = w->t[0] + (int)(((long long)w->m[0][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[0][2] * fwd) >> 12);
	z = w->t[2] + (int)(((long long)w->m[2][0] * lat * s) >> 12)
	           + (int)(((long long)w->m[2][2] * fwd) >> 12);

	g.vx = x;
	g.vz = z;
	g.vy = 0;

	yaw = (cp->hd.direction + s * 1024) & 0xfff;

	jer_npc_set_world((JerNpc*)pPed, x, -MapHeight(&g) - 130, z, yaw);
}

static void cd2CrewSpawnSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp)
{
	LPPEDESTRIAN pPed;

	if (c->ped[i] != NULL)
		return;

	if (!cd2CrewNear(cp))
		return;

	pPed = (LPPEDESTRIAN)jer_npc_spawn_model(TANNER_MODEL, cp->hd.where.t[0], cp->hd.where.t[2]);

	if (pPed == NULL)
	{
		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] crew: car=%d side=%d skip: ped pool full\n", cp->id, i);
		return;
	}

	// A crew ped is nobody's player: -1 keeps the player-only pose/draw hooks
	// (which gate on padId >= 0) and the shadow off.
	pPed->padId = -1;
	pPed->index = -1;
	pPed->head_rot = 0;
	pPed->flags = 0;

	c->ped[i] = (JerNpc*)pPed;
	c->state[i] = CD2_CREW_OUT;
	c->frame[i] = 0;

	jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, 0);
	cd2CrewPlace(pPed, cp, i);

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] crew: car=%d side=%d out (get out)\n", cp->id, i);
}

static void cd2CrewDespawnSide(CD2_CREW_CAR* c, int i)
{
	if (c->ped[i] != NULL)
		jer_npc_despawn(c->ped[i]);

	c->ped[i] = NULL;
	c->state[i] = CD2_CREW_IN;
	c->frame[i] = 0;
}

static void cd2CrewUpdateSide(CD2_CREW_CAR* c, int i, const CAR_DATA* cp)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)c->ped[i];
	int wantOut = c->hold[i] > 0;

	// the ped may have been destroyed under us - the list is authoritative
	if (pPed != NULL && !cd2CrewPedAlive(pPed))
	{
		c->ped[i] = NULL;
		c->state[i] = CD2_CREW_IN;
		c->frame[i] = 0;
		pPed = NULL;
	}

	if (wantOut)
	{
		if (pPed == NULL)
		{
			cd2CrewSpawnSide(c, i, cp);
			pPed = (LPPEDESTRIAN)c->ped[i];
		}
		else if (c->state[i] == CD2_CREW_INANIM)
		{
			// fire resumed mid get-in: lean straight back out
			c->state[i] = CD2_CREW_OUT;
			c->frame[i] = 0;
		}

		if (pPed != NULL)
		{
			if (c->state[i] == CD2_CREW_OUT)
			{
				if (c->frame[i] < CD2_CREW_GETOUT_LAST)
					c->frame[i]++;

				jer_npc_set_action(c->ped[i], PED_ACTION_GETOUTCAR, c->frame[i]);
			}

			cd2CrewPlace(pPed, cp, i);
		}
	}
	else if (pPed != NULL)
	{
		if (c->state[i] != CD2_CREW_INANIM)
		{
			c->state[i] = CD2_CREW_INANIM;
			c->frame[i] = 0;

			if (gCd2Cfg.debugLog)
				printInfo("[combatd2] crew: car=%d side=%d in (get in)\n", cp->id, i);
		}

		c->frame[i]++;

		if (c->frame[i] >= CD2_CREW_GETIN_LAST)
		{
			cd2CrewDespawnSide(c, i);
		}
		else
		{
			jer_npc_set_action(c->ped[i], PED_ACTION_GETINCAR, c->frame[i]);
			cd2CrewPlace(pPed, cp, i);
		}
	}
}

// ---------------------------------------------------------------------------
// JER_EVENT_FRAME: age the holds, then drive every car's crew.
//
// Registered at priority 1 so it runs AFTER the weapon FRAME hook (priority 0)
// that fires the weapons: a shot fired this frame is still fully "out" for the
// ped render this frame, and only ages from the next frame on.
// ---------------------------------------------------------------------------
static int cd2CrewOnFrame(void* ud, void* args)
{
	int i, side;
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled)
		return JER_RESULT_CONTINUE;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (gCrew[i].hold[CD2_CREW_SIDE_DRIVER] > 0)
			gCrew[i].hold[CD2_CREW_SIDE_DRIVER]--;

		if (gCrew[i].hold[CD2_CREW_SIDE_GUNNER] > 0)
			gCrew[i].hold[CD2_CREW_SIDE_GUNNER]--;

		if (!cd2CrewOwned(cp))
		{
			// not ours: make sure nothing is left hanging out
			if (gCd2Cfg.debugLog && (gCrew[i].hold[0] > 0 || gCrew[i].hold[1] > 0))
				printInfo("[combatd2] crew: car=%d skip: not owned (controlType=%d)\n",
					i, cp->controlType);

			for (side = 0; side < 2; side++)
				if (gCrew[i].ped[side] != NULL)
					cd2CrewDespawnSide(&gCrew[i], side);

			continue;
		}

		for (side = 0; side < 2; side++)
			cd2CrewUpdateSide(&gCrew[i], side, cp);
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// JER_EVENT_GAME_START: a fresh level. InitPedestrians has ALREADY reset the
// ped pool by the time this fires, so every stored pointer is dangling - forget
// them without touching the (rebuilt) pool.
// ---------------------------------------------------------------------------
static int cd2CrewOnGameStart(void* ud, void* args)
{
	int i, side;
	(void)ud;
	(void)args;

	for (i = 0; i < MAX_CARS; i++)
	{
		for (side = 0; side < 2; side++)
		{
			gCrew[i].hold[side] = 0;
			gCrew[i].ped[side] = NULL;
			gCrew[i].state[side] = CD2_CREW_IN;
			gCrew[i].frame[side] = 0;
		}
	}

	return JER_RESULT_CONTINUE;
}

// ---------------------------------------------------------------------------
// Registration (called once by jer_module_combatd2_entry in combatd2.c)
// ---------------------------------------------------------------------------
void cd2CrewRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2CrewOnFrame, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2CrewOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[combatd2] mounted crew registered (SDK v%d)\n", ctx->sdkVersion);
}
