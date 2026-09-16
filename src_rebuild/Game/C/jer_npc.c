/* jer_npc.c — the game-side implementations of the jer_npc scaffolding.
 * See jer_npc.h. The GAME compiles this (only the game sees LPPEDESTRIAN);
 * the JERICHO core only declares the API. */

#include "jer_npc.h"

#include "driver2.h"
#include "pedest.h"
#include "motion_c.h"	/* SetupPedMotionData */
#include "civ_ai.h"
#include "dr2roads.h"
#include "replays.h"	/* MAX_PLACED_PEDS */

/* A parked ped's state function: it does nothing, so the ped neither walks nor
 * advances its animation. It is installed into BOTH state slots, so whichever
 * one the pedestrian updater calls the ped stays put. */
static void jer_npc_frozen_state(LPPEDESTRIAN pPed)
{
	(void)pPed;
}

static void jer_npc_park_internal(LPPEDESTRIAN pPed)
{
	pPed->speed = 0;
	pPed->doing_turn = 0;
	pPed->finished_turn = 0;
	pPed->velocity.vx = 0;
	pPed->velocity.vy = 0;
	pPed->velocity.vz = 0;
	pPed->target.vx = pPed->position.vx;
	pPed->target.vy = pPed->position.vy;
	pPed->target.vz = pPed->position.vz;
	pPed->fpRestState = jer_npc_frozen_state;
	pPed->fpAgitatedState = jer_npc_frozen_state;
}

/* ---------------------------------------------------------------------------
 * Ownership (see jer_npc.h). A ped a module spawned through this API is
 * "owned": the ped-pose hooks (JER_EVENT_PED_POSE / PED_SKELETON) only fire
 * for owned peds. Spawning marks, despawn unmarks; the query also checks the
 * ped is still on the engine's live list (pUsedPeds), so an entry whose ped
 * the engine destroyed behind our back reports unowned instead of matching
 * whatever pedestrian reused the slot.
 * ------------------------------------------------------------------------- */
#define JER_NPC_MAX_OWNED	32
static LPPEDESTRIAN jer_npc_owned_peds[JER_NPC_MAX_OWNED];

static int jer_npc_ped_alive(LPPEDESTRIAN pPed)
{
	LPPEDESTRIAN q = pUsedPeds;

	while (q != NULL)
	{
		if (q == pPed)
			return 1;

		q = q->pNext;
	}

	return 0;
}

static void jer_npc_own_add(LPPEDESTRIAN pPed)
{
	int i;

	if (pPed == NULL)
		return;

	for (i = 0; i < JER_NPC_MAX_OWNED; i++)
	{
		if (jer_npc_owned_peds[i] == pPed)
			return;		/* already owned */
	}

	for (i = 0; i < JER_NPC_MAX_OWNED; i++)
	{
		if (jer_npc_owned_peds[i] == NULL)
		{
			jer_npc_owned_peds[i] = pPed;
			return;
		}
	}

	/* registry full: the ped simply stays unowned (it just won't be poseable) */
}

static void jer_npc_own_remove(LPPEDESTRIAN pPed)
{
	int i;

	for (i = 0; i < JER_NPC_MAX_OWNED; i++)
	{
		if (jer_npc_owned_peds[i] == pPed)
			jer_npc_owned_peds[i] = NULL;
	}
}

int jer_npc_owned(const void* ped)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)ped;
	int i;

	if (pPed == NULL)
		return 0;

	for (i = 0; i < JER_NPC_MAX_OWNED; i++)
	{
		if (jer_npc_owned_peds[i] == pPed)
			return jer_npc_ped_alive(pPed);
	}

	return 0;
}

/* Ring radii / directions used to search outward from a requested spawn point
 * for walkable pavement. Snapping to the nearest walkable spot keeps a ped
 * from being created out of bounds (off the road graph), which the pavement
 * test - the same one the engine's own ped spawner uses - rejects. */
static const int jer_npc_ring[] = { 0, 64, 128, 256, 512, 1024 };
static const int jer_npc_dir[8][2] =
{
	{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
	{ 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }
};

/* Fill `out` with the nearest walkable pavement to (x, z), snapping Y to the
 * ground. Returns 1 on success, 0 if nothing walkable was found nearby. */
static int jer_npc_snap_to_road(int x, int z, VECTOR* out)
{
	int r, d;

	for (r = 0; r < (int)(sizeof(jer_npc_ring) / sizeof(jer_npc_ring[0])); r++)
	{
		int rad = jer_npc_ring[r];
		int n = (rad == 0) ? 1 : 8;

		for (d = 0; d < n; d++)
		{
			VECTOR p;

			p.vx = x + jer_npc_dir[d][0] * rad;
			p.vz = z + jer_npc_dir[d][1] * rad;
			p.vy = 0;
			p.vy = -MapHeight(&p);

			if (IsPavement(p.vx, p.vy, p.vz, NULL))
			{
				*out = p;
				return 1;
			}
		}
	}

	return 0;
}

JerNpc* jer_npc_spawn(int x, int z)
{
	LPPEDESTRIAN pPed;
	VECTOR pos;

	/* Snap the requested point to the nearest walkable pavement so the ped
	 * never spawns out of bounds. If there is nothing walkable nearby, keep
	 * the request but still ground it. */
	pos.vx = x;
	pos.vz = z;
	pos.vy = 0;

	if (!jer_npc_snap_to_road(x, z, &pos))
	{
		pos.vx = x;
		pos.vz = z;
		pos.vy = -MapHeight(&pos);
	}

	pPed = CreatePedestrian();

	if (pPed == NULL)
		return NULL;

	pPed->position.vx = pos.vx;
	pPed->position.vz = pos.vz;
	pPed->position.vy = pos.vy - 130;

	pPed->pedType = CIVILIAN;
	pPed->dir.vx = 0;
	pPed->dir.vy = 0;
	pPed->dir.vz = 0;
	pPed->type = PED_ACTION_WALK;
	pPed->flags = 0;

	/* pPed->motion must point at the motion block for pPed->type, or the
	 * draw path reads the pose out of a stale/NULL buffer. */
	SetupPedMotionData(pPed);

	/* owned: this ped becomes eligible for the JERICHO ped-pose hooks */
	jer_npc_own_add(pPed);

	return (JerNpc*)pPed;
}

void jer_npc_despawn(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed != NULL)
	{
		jer_npc_own_remove(pPed);
		DestroyPedestrian(pPed);
	}
}

void jer_npc_face(JerNpc* n, int heading)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	pPed->dir.vy = heading & 0xfff;
	pPed->head_rot = 0;
}

void jer_npc_set_speed(JerNpc* n, int speed)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	pPed->speed = (char)speed;
}

int jer_npc_speed(const JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	return pPed ? pPed->speed : 0;
}

void jer_npc_move_to(JerNpc* n, int x, int z, int speed)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	/* face the target, then walk/run toward it */
	pPed->dir.vy = ratan2(x - pPed->position.vx, z - pPed->position.vz);
	pPed->speed = (char)speed;
	pPed->type = PED_ACTION_WALK;

	/* the civ personality table is pedest-internal; the walk/run states
	 * are driven by the speed + type fields alone for the scaffold */
}

void jer_npc_stop(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	pPed->speed = 0;
	pPed->type = PED_ACTION_WALK;	/* speed 0 = standing */
	pPed->fpAgitatedState = NULL;
}

int jer_npc_leave_car(JerNpc* n)
{
	(void)n;	/* STUB: the police bail-out is future work */

	return 0;
}

JerNpc* jer_npc_spawn_model(int pedModel, int x, int z)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)jer_npc_spawn(x, z);

	if (pPed != NULL)
		pPed->pedType = (char)pedModel;

	return (JerNpc*)pPed;
}

void jer_npc_park(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed != NULL)
		jer_npc_park_internal(pPed);
}

void jer_npc_set_action(JerNpc* n, int action, int frame)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	jer_npc_park_internal(pPed);

	pPed->type = (char)action;
	SetupPedMotionData(pPed);	/* pPed->motion = MotionCaptureData[action] */
	pPed->frame1 = (char)frame;
}

void jer_npc_set_world(JerNpc* n, int x, int y, int z, int yaw)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	pPed->position.vx = x;
	pPed->position.vy = y;
	pPed->position.vz = z;
	pPed->dir.vy = (short)(yaw & 0xfff);
}

void jer_npc_set_orient(JerNpc* n, int pitch, int yaw, int roll)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	/* newRotateBones builds the root matrix as RotMatrixYXZ(pPed->dir), so all
	 * three components are live - not just the yaw. */
	pPed->dir.vx = (short)(pitch & 0xfff);
	pPed->dir.vy = (short)(yaw & 0xfff);
	pPed->dir.vz = (short)(roll & 0xfff);
}
