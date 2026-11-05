/* jer_npc.c — the game-side implementations of the jer_npc scaffolding.
 * See jer_npc.h. The GAME compiles this (only the game sees LPPEDESTRIAN);
 * the JERICHO core only declares the API. */

#include "jer_npc.h"

#include <string.h>	/* memset (health table) */

#include "driver2.h"
#include "pedest.h"
#include "motion_c.h"	/* SetupPedMotionData */
#include "civ_ai.h"
#include "dr2roads.h"
#include "replays.h"	/* MAX_PLACED_PEDS */
#include "debris.h"		/* Setup_Sparks / Setup_Debris / Setup_Smoke (jer_fx_impact) */

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

	/* A module-spawned ped is never a PLAYER's ped. CreatePedestrian reuses a
	 * pooled slot and leaves `padId` as the previous occupant had it, so a stray
	 * value here would (a) read as "someone's character" to a padId-keyed scan and
	 * (b) make AnimatePed write player[ABS(padId)].pos -- at padId 0 that
	 * overwrites the LOCAL player's position every frame. -1 is the safe "no
	 * player" value: ABS(-1) = 1, the unused slot. */
	pPed->padId = -1;

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

	/* every jer NPC starts at full health (a reused pool slot does not
	 * inherit the previous occupant's) */
	jer_npc_health_forget(pPed);
	jer_npc_set_health(pPed, JER_NPC_HEALTH_DEFAULT, JER_NPC_HEALTH_DEFAULT);

	return (JerNpc*)pPed;
}

void jer_npc_despawn(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed != NULL)
	{
		jer_npc_own_remove(pPed);
		jer_npc_health_forget(pPed);
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

void jer_npc_set_reverse_anim(JerNpc* n, int on)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	if (on)
		pPed->flags |= 4;
	else
		pPed->flags &= ~4;
}

JerNpc* jer_npc_nearest(int x, int y, int z, int radius)
{
	LPPEDESTRIAN pPed, best = NULL;
	int bestD = 0x7fffffff;

	for (pPed = pUsedPeds; pPed != NULL; pPed = pPed->pNext)
	{
		int dx, dy, dz, d;

		if (pPed->padId >= 0)
			continue;	/* a player */

		if (pPed->pedType == TANNER_MODEL && !jer_npc_owned(pPed))
			continue;	/* an unowned player-model ped */

		dx = pPed->position.vx - x;
		dy = pPed->position.vy - y;	/* position.vy is already the render (y-down) frame */
		dz = pPed->position.vz - z;

		if (dx < -radius || dx > radius || dz < -radius || dz > radius ||
			dy < -2 * radius || dy > 2 * radius)
			continue;

		d = dx * dx + dz * dz;

		if (d <= radius * radius && d < bestD)
		{
			bestD = d;
			best = pPed;
		}
	}

	return (JerNpc*)best;
}

/* ---------------------------------------------------------------------------
 * health: see jer_npc.h
 * ------------------------------------------------------------------------- */
#define JER_NPC_HEALTH_SLOTS	64

typedef struct JerNpcHealth
{
	LPPEDESTRIAN ped;
	int health;
	int max;
} JerNpcHealth;

static JerNpcHealth jer_npc_hp[JER_NPC_HEALTH_SLOTS];
static int jer_npc_hp_next = 0;

static JerNpcHealth* jer_npc_hp_find(const void* ped, int create)
{
	JerNpcHealth* freeSlot = NULL;
	JerNpcHealth* h;
	int i;

	if (ped == NULL)
		return NULL;

	for (i = 0; i < JER_NPC_HEALTH_SLOTS; i++)
	{
		h = &jer_npc_hp[i];

		if (h->ped == NULL)
		{
			if (freeSlot == NULL)
				freeSlot = h;
			continue;
		}

		if (h->ped == (LPPEDESTRIAN)ped)
			return h;
	}

	if (!create)
		return NULL;

	if (freeSlot == NULL)
	{
		/* full: drop entries whose ped the engine has destroyed, then
		 * fall back to overwriting round-robin */
		for (i = 0; i < JER_NPC_HEALTH_SLOTS; i++)
		{
			h = &jer_npc_hp[i];

			if (h->ped->padId < 0 && !jer_npc_ped_alive(h->ped))
			{
				h->ped = NULL;
				if (freeSlot == NULL)
					freeSlot = h;
			}
		}

		if (freeSlot == NULL)
		{
			freeSlot = &jer_npc_hp[jer_npc_hp_next];
			jer_npc_hp_next = (jer_npc_hp_next + 1) % JER_NPC_HEALTH_SLOTS;
		}
	}

	freeSlot->ped = (LPPEDESTRIAN)ped;
	freeSlot->health = JER_NPC_HEALTH_DEFAULT;
	freeSlot->max = JER_NPC_HEALTH_DEFAULT;

	return freeSlot;
}

int jer_npc_health(const void* ped)
{
	JerNpcHealth* h = jer_npc_hp_find(ped, 0);

	if (ped == NULL)
		return 0;

	return h ? h->health : JER_NPC_HEALTH_DEFAULT;
}

int jer_npc_health_max(const void* ped)
{
	JerNpcHealth* h = jer_npc_hp_find(ped, 0);

	if (ped == NULL)
		return 0;

	return h ? h->max : JER_NPC_HEALTH_DEFAULT;
}

void jer_npc_set_health(void* ped, int health, int max)
{
	JerNpcHealth* h = jer_npc_hp_find(ped, 1);

	if (h == NULL)
		return;

	if (max > 0)
		h->max = max;

	if (health < 0)
		health = 0;
	if (health > h->max)
		health = h->max;

	h->health = health;
}

int jer_npc_damage(void* ped, int amount)
{
	JerNpcHealth* h = jer_npc_hp_find(ped, 1);

	if (h == NULL)
		return 0;

	h->health -= amount;

	if (h->health < 0)
		h->health = 0;
	if (h->health > h->max)
		h->health = h->max;

	return h->health;
}

void jer_npc_health_forget(const void* ped)
{
	JerNpcHealth* h = jer_npc_hp_find(ped, 0);

	if (h != NULL)
		h->ped = NULL;
}

void jer_npc_health_reset_all(void)
{
	memset(jer_npc_hp, 0, sizeof(jer_npc_hp));
	jer_npc_hp_next = 0;
}

int jer_npc_get_pos(const JerNpc* n, int out[3])
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL || out == NULL)
		return 0;

	out[0] = pPed->position.vx;
	out[1] = pPed->position.vy;
	out[2] = pPed->position.vz;

	return 1;
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

/* Drive a ped like a mirrored player: face `heading`, move at `speed` (negative =
 * backing up, as Tanner does), and pick the matching motion clip. move_to/stop
 * only set `type`, never the motion data or the reverse-playback flag (4), so a
 * stand-in kept whatever clip and direction it spawned/pooled with -- which is
 * the "stuck walking backwards" remote Tanner. Clip changes only on a gait
 * change so the frame counter keeps running. */
void jer_npc_set_gait(JerNpc* n, int heading, int speed)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;
	int type;

	if (pPed == NULL)
		return;

	if (speed == 0)
		type = PED_ACTION_BACK;		/* the stock idle ("DoNowt") clip */
	else if (speed >= 30)
		type = PED_ACTION_RUN;
	else
		type = PED_ACTION_WALK;

	pPed->fpAgitatedState = NULL;
	pPed->dir.vy = heading & 0xfff;
	pPed->dir.vz = 0;
	pPed->head_rot = 0;
	pPed->speed = (char)speed;

	if (speed < 0)
		pPed->flags |= 4;		/* play the walk clip in reverse */
	else
		pPed->flags &= ~4;

	if (pPed->type != type || pPed->motion == NULL)
	{
		pPed->type = (char)type;
		SetupPedMotionData(pPed);
	}

	pPed->flags |= 0x10;
}

/* pedest.c, and deliberately not in a header: this is the animation/integration
 * pass a ped's own state function calls. */
extern void AnimatePed(LPPEDESTRIAN pPed);

void jer_npc_tick(JerNpc* n)
{
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)n;

	if (pPed == NULL)
		return;

	/* A PARKED ped is held exactly where the module put it -- both its state slots
	 * are the frozen no-op. Ticking it would move it out from under that. */
	if (pPed->fpRestState == jer_npc_frozen_state)
		return;

	/* AnimatePed is the whole reason a stock pedestrian walks: it advances the
	 * walk/run/idle animation from `speed` AND integrates `position` from speed and
	 * `dir`. Two of its side effects are worth knowing for a stand-in:
	 *   - it plays Tanner footstep sounds for a TANNER_MODEL ped (fine -- a walking
	 *     character should make a noise);
	 *   - for any NON-civilian ped it writes player[ABS(padId)].pos, which for a
	 *     stand-in (padId = -1) lands on the unused slot 1. ABS(-1)=1 is why the
	 *     stand-in's padId MUST stay negative: at 0 it would overwrite the LOCAL
	 *     player's own position every frame. */
	AnimatePed(pPed);
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
/* ---------------------------------------------------------------------------
 * jer_fx_impact (see jer_npc.h): a bullet-style impact built from the same
 * particle calls a car collision uses (bcollide.c: Setup_Sparks + Setup_Debris)
 * plus a small black smoke puff as the dark impact mark. */
void jer_fx_impact(int x, int y, int z, int vx, int vz, int kind)
{
	VECTOR pos, vel, drift;

	pos.vx = x;
	pos.vy = y;
	pos.vz = z;
	pos.pad = 0;

	vel.vx = vx;
	vel.vy = -17;		/* the car-collision spark lift (bcollide.c) */
	vel.vz = vz;
	vel.pad = 0;

	drift.vx = drift.vy = drift.vz = drift.pad = 0;

	Setup_Sparks(&pos, &vel, kind == JER_FX_IMPACT_PED ? 3 : 6, 0);

	if (kind != JER_FX_IMPACT_PED)
		Setup_Debris(&pos, &vel, 4, 0);

	Setup_Smoke(&pos, 20, kind == JER_FX_IMPACT_PED ? 80 : 140, SMOKE_BLACK, 0, &drift, 0);
}
