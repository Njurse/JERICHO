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

JerNpc* jer_npc_spawn(int x, int z)
{
	LPPEDESTRIAN pPed;
	VECTOR pos;

	pPed = CreatePedestrian();

	if (pPed == NULL)
		return NULL;

	pos.vx = x;
	pos.vz = z;
	pos.vy = 0;
	pPed->position.vx = x;
	pPed->position.vz = z;
	pPed->position.vy = -130 - MapHeight(&pos);

	pPed->pedType = CIVILIAN;
	pPed->dir.vx = 0;
	pPed->dir.vy = 0;
	pPed->dir.vz = 0;
	pPed->type = PED_ACTION_WALK;
	pPed->flags = 0;

	return (JerNpc*)pPed;
}

void jer_npc_despawn(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed != NULL)
		DestroyPedestrian(pPed);
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
