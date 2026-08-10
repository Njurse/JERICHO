/* jer_npc.c — the game-side implementations of the jer_npc scaffolding.
 * See jer_npc.h. The GAME compiles this (only the game sees LPPEDESTRIAN);
 * the JERICHO core only declares the API. */

#include "jer_npc.h"

#include "driver2.h"
#include "pedest.h"
#include "civ_ai.h"
#include "dr2roads.h"
#include "replays.h"	/* MAX_PLACED_PEDS */

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
	pPed->fpRestState = fpPedPersonalityFunctions[12];

	return (JerNpc*)pPed;
}

void jer_npc_despawn(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)(n ? n->ped : NULL);

	if (pPed != NULL)
		DestroyPedestrian(pPed);
}

void jer_npc_face(JerNpc* n, int heading)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)(n ? n->ped : NULL);

	if (pPed == NULL)
		return;

	pPed->dir.vy = heading & 0xfff;
	pPed->head_rot = 0;
}

void jer_npc_set_speed(JerNpc* n, int speed)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)(n ? n->ped : NULL);

	if (pPed == NULL)
		return;

	pPed->speed = (char)speed;
}

int jer_npc_speed(const JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)(n ? n->ped : NULL);

	return pPed ? pPed->speed : 0;
}

void jer_npc_move_to(JerNpc* n, int x, int z, int speed)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)(n ? n->ped : NULL);

	if (pPed == NULL)
		return;

	/* face the target, then walk/run toward it */
	pPed->dir.vy = ratan2(x - pPed->position.vx, z - pPed->position.vz);
	pPed->speed = (char)speed;
	pPed->type = PED_ACTION_WALK;

	if (pPed->fpAgitatedState == NULL)
		pPed->fpAgitatedState = fpPedPersonalityFunctions[2];	/* runner */
}

void jer_npc_stop(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)(n ? n->ped : NULL);

	if (pPed == NULL)
		return;

	pPed->speed = 0;
	pPed->type = PED_ACTION_STAND;
	pPed->fpAgitatedState = NULL;
}

int jer_npc_leave_car(JerNpc* n)
{
	(void)n;	/* STUB: the police bail-out is future work */

	return 0;
}
