// ai/opponent.c — Combat D2 PROTOTYPE OPPONENT AI.
//
// A single opponent car spawned beside the player. It is driven through the
// SAME combatd2 input path a human uses (it writes cp->thrust / cp->wheel_angle
// and the combatd2 handling model moves it), so it feels exactly like the
// player's car. Behaviours:
//
//   HUNT    - chase the player, fire the MG when lined up
//   FLEE    - run from the player when badly damaged
//   RECOVER - totalled: come to rest
//   WANDER  - explore (holding a heading, looking for pickups/opponents)
//   EVADE   - an OVERLAY (rides on top of whatever else): on detecting an
//             incoming shot it dodges perpendicular to that shot
//
// Obstacle handling: a lineClear() probe ahead steers it around buildings, and
// it nudges away from other cars it is about to hit. Spawn is right beside the
// player so it is easy to watch.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "civ_ai.h"
#include "players.h"
#include "objcoll.h"
#include "convert.h"
#include "system.h"
#include "pres.h"
#include "dr2math.h"
#include "jericho.h"
#include "jer_events.h"
#include "jer_math.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"
#include "ai/ai.h"

#include <string.h>

#define CD2_AI_SPAWN_OFFSET	300	// how far beside the player to spawn
#define CD2_AI_HUNT_RANGE	3200	// start hunting within this
#define CD2_AI_LOOK		560	// look-ahead probe distance
#define CD2_AI_PROBE_ANG	400	// ~35 deg side probes
#define CD2_AI_AVOID_STEER	150	// steer nudge to dodge something
#define CD2_AI_STEER_DIV	6	// heading error -> wheel_angle divisor
#define CD2_AI_EVADE_FRAMES	45	// ~1.5 s of evasive driving
#define CD2_AI_HURT		6000	// totalDamage above which it flees
#define CD2_AI_FIRE_RANGE	2600	// MG range when hunting
#define CD2_AI_FIRE_COOLDOWN	10	// frames between AI MG shots

static int sAiCarId = -1;
static int sSpawned;
static int sState = CD2_AI_WANDER;
static int sStateTimer;
static int sEvade;
static int sFireTimer;
static int sWanderHeading;
static int sWanderTimer;

static void cd2AiPointAt(const CAR_DATA* cp, int heading, int dist, VECTOR* out)
{
	out->vx = cp->hd.where.t[0] + (int)(((long long)RCOS(heading) * dist) >> 12);
	out->vy = cp->hd.where.t[1];
	out->vz = cp->hd.where.t[2] + (int)(((long long)RSIN(heading) * dist) >> 12);
}

static void cd2AiCarPos(const CAR_DATA* cp, VECTOR* out)
{
	out->vx = cp->hd.where.t[0];
	out->vy = cp->hd.where.t[1];
	out->vz = cp->hd.where.t[2];
}

static int cd2AiSpawn(void)
{
	CAR_DATA* pcp = NULL;
	CAR_DATA* slot = NULL;
	LONGVECTOR4 pos;
	EXTRA_CIV_DATA dat;
	int i;

	if (!cd2WpnPlayerCar(&pcp))
		return 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_NONE)
		{
			slot = &car_data[i];
			break;
		}
	}

	if (slot == NULL)
		return 0;

	// spawn beside the player, along the player's right axis
	pos[0] = pcp->hd.where.t[0] + (int)(((long long)pcp->hd.where.m[0][0] * CD2_AI_SPAWN_OFFSET) >> 12);
	pos[1] = pcp->hd.where.t[1];
	pos[2] = pcp->hd.where.t[2] + (int)(((long long)pcp->hd.where.m[2][0] * CD2_AI_SPAWN_OFFSET) >> 12);
	pos[3] = 0;

	memset(&dat, 0, sizeof(dat));
	dat.palette = 0;

	InitCar(slot, pcp->hd.direction, &pos, CONTROL_TYPE_CIV_AI, pcp->ap.model, 0, (char*)&dat);

	sAiCarId = slot->id;
	sState = CD2_AI_WANDER;
	sStateTimer = 0;
	sEvade = 0;
	sFireTimer = 0;
	sWanderHeading = pcp->hd.direction;
	sWanderTimer = 60;

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] AI opponent spawned (car=%d)\n", sAiCarId);

	return 1;
}

static void cd2AiDrive(CAR_DATA* cp)
{
	CAR_DATA* pcp = NULL;
	VECTOR tpos;
	int desired, diff, steer, thrust;
	int fx, fz, rx, rz;
	int i;

	if (cd2CarTotaled(cp))
	{
		cp->thrust = 0;
		cp->wheel_angle = 0;
		cp->handbrake = 0;
		cp->wheelspin = 0;
		return;
	}

	cd2WpnPlayerCar(&pcp);

	fx = cp->hd.where.m[0][2];
	fz = cp->hd.where.m[2][2];
	rx = cp->hd.where.m[0][0];
	rz = cp->hd.where.m[2][0];

	// --- evade overlay: any incoming shot? ---
	if (cd2WpnIncomingThreat(cp, &tpos, NULL))
	{
		sEvade = CD2_AI_EVADE_FRAMES;

		if (gCd2Cfg.debugLog)
			printInfo("[combatd2] AI evade (car=%d)\n", cp->id);
	}

	if (sEvade > 0)
		sEvade--;

	// --- base state (forced, or decided from conditions) ---
	if (gCd2Cfg.aiForceState != CD2_AI_AUTO)
	{
		sState = gCd2Cfg.aiForceState;
	}
	else if (--sStateTimer <= 0)
	{
		long long d2 = 0;

		if (pcp != NULL)
		{
			long long px = pcp->hd.where.t[0] - cp->hd.where.t[0];
			long long pz = pcp->hd.where.t[2] - cp->hd.where.t[2];
			d2 = px * px + pz * pz;
		}

		if (pcp != NULL && cp->totalDamage > CD2_AI_HURT &&
		    d2 < (long long)CD2_AI_HUNT_RANGE * CD2_AI_HUNT_RANGE)
			sState = CD2_AI_FLEE;
		else if (pcp != NULL && d2 < (long long)CD2_AI_HUNT_RANGE * CD2_AI_HUNT_RANGE)
			sState = CD2_AI_HUNT;
		else
			sState = CD2_AI_WANDER;

		sStateTimer = 45;
	}

	// --- desired heading ---
	if (sEvade > 0)
	{
		// dodge toward the side the threat is NOT on
		int side = (tpos.vx - cp->hd.where.t[0]) * rx + (tpos.vz - cp->hd.where.t[2]) * rz;

		desired = cp->hd.direction + ((side > 0) ? -1024 : 1024);
	}
	else if (sState == CD2_AI_HUNT && pcp != NULL)
	{
		desired = ratan2(pcp->hd.where.t[0] - cp->hd.where.t[0],
				 pcp->hd.where.t[2] - cp->hd.where.t[2]);
	}
	else if (sState == CD2_AI_FLEE && pcp != NULL)
	{
		desired = ratan2(cp->hd.where.t[0] - pcp->hd.where.t[0],
				 cp->hd.where.t[2] - pcp->hd.where.t[2]);
	}
	else
	{
		if (--sWanderTimer <= 0)
		{
			sWanderHeading = (sWanderHeading + 900 + Random2(1400)) & 0xfff;
			sWanderTimer = 60 + Random2(90);
		}

		desired = sWanderHeading;
	}

	// --- steering ---
	diff = desired - cp->hd.direction;
	while (diff > 2048) diff -= 4096;
	while (diff < -2048) diff += 4096;

	steer = diff / CD2_AI_STEER_DIV;

	// obstacle avoidance: scenery/wall ahead (lineClear: 0 = blocked)
	{
		VECTOR carPos, ahead, leftP, rightP;
		int clearAhead, clearL, clearR;

		cd2AiCarPos(cp, &carPos);
		cd2AiPointAt(cp, cp->hd.direction, CD2_AI_LOOK, &ahead);
		cd2AiPointAt(cp, cp->hd.direction + CD2_AI_PROBE_ANG, CD2_AI_LOOK, &leftP);
		cd2AiPointAt(cp, cp->hd.direction - CD2_AI_PROBE_ANG, CD2_AI_LOOK, &rightP);

		clearAhead = lineClear(&carPos, &ahead);
		clearL = lineClear(&carPos, &leftP);
		clearR = lineClear(&carPos, &rightP);

		if (clearAhead == 0)
		{
			if (clearL && !clearR)
				steer += CD2_AI_AVOID_STEER;
			else if (clearR && !clearL)
				steer -= CD2_AI_AVOID_STEER;
			else
				steer += (steer >= 0) ? CD2_AI_AVOID_STEER : -CD2_AI_AVOID_STEER;
		}
	}

	// obstacle avoidance: cars about to be hit
	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* o = &car_data[i];
		int dx, dz, aheadDot, side;

		if (o == cp || o->controlType == CONTROL_TYPE_NONE)
			continue;

		dx = o->hd.where.t[0] - cp->hd.where.t[0];
		dz = o->hd.where.t[2] - cp->hd.where.t[2];

		if (dx * dx + dz * dz > CD2_AI_LOOK * CD2_AI_LOOK)
			continue;

		aheadDot = dx * fx + dz * fz;
		if (aheadDot <= 0)
			continue;	// behind us

		side = dx * rx + dz * rz;	// > 0 = obstacle on our right
		steer += (side > 0) ? -CD2_AI_AVOID_STEER : CD2_AI_AVOID_STEER;
	}

	steer = jer_clamp_int(steer, -CD2_STEER_MAX, CD2_STEER_MAX);

	// --- throttle ---
	thrust = CD2_TMB_THRUST;

	if (sEvade == 0)
	{
		if (ABS(diff) > 1700)
			thrust = CD2_TMB_THRUST / 3;
		else if (ABS(diff) > 1000)
			thrust = CD2_TMB_THRUST / 2;
	}

	cp->wheel_angle = (short)steer;
	cp->thrust = (short)thrust;
	cp->handbrake = 0;
	cp->wheelspin = 0;

	// --- offensive: MG the player when lined up (hunt only, not while dodging) ---
	if (sFireTimer > 0)
		sFireTimer--;

	if (sState == CD2_AI_HUNT && sEvade == 0 && pcp != NULL && sFireTimer == 0)
	{
		long long dx = pcp->hd.where.t[0] - cp->hd.where.t[0];
		long long dz = pcp->hd.where.t[2] - cp->hd.where.t[2];

		if (dx * dx + dz * dz < (long long)CD2_AI_FIRE_RANGE * CD2_AI_FIRE_RANGE &&
		    ABS(diff) < 260)
		{
			const CD2_WEAPON_DEF* mg = cd2WpnDef(CD2_WID_MG);

			if (mg != NULL && mg->fire != NULL)
			{
				mg->fire(cp);
				sFireTimer = CD2_AI_FIRE_COOLDOWN;
			}
		}
	}
}

// --- hooks -----------------------------------------------------------------

static int cd2AiOnFrame(void* ud, void* args)
{
	(void)ud;
	(void)args;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiOpponent)
		return JER_RESULT_CONTINUE;

	// drop a destroyed/removed opponent so the id doesn't go stale
	if (sAiCarId >= 0 &&
	    (sAiCarId >= MAX_CARS || car_data[sAiCarId].controlType == CONTROL_TYPE_NONE))
		sAiCarId = -1;

	if (sAiCarId < 0 && !sSpawned)
		cd2AiSpawn();

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnCarStep(void* ud, void* args)
{
	JER_ARGS_CAR_STEP* a = (JER_ARGS_CAR_STEP*)args;
	CAR_DATA* cp = (CAR_DATA*)a->car;
	(void)ud;

	if (!gCd2Cfg.enabled || !gCd2Cfg.aiOpponent || sAiCarId < 0)
		return JER_RESULT_CONTINUE;

	if (cp->id == sAiCarId)
		cd2AiDrive(cp);

	return JER_RESULT_CONTINUE;
}

static int cd2AiOnGameStart(void* ud, void* args)
{
	(void)ud;
	(void)args;

	sAiCarId = -1;
	sSpawned = 0;
	sState = CD2_AI_WANDER;
	sStateTimer = 0;
	sEvade = 0;
	sFireTimer = 0;

	// spawn as soon as the frame hook sees a player car (the player car may
	// not exist yet at GAME_START)
	return JER_RESULT_CONTINUE;
}

int cd2AiActive(void)
{
	return (sAiCarId >= 0) ? 1 : 0;
}

const char* cd2AiStateName(void)
{
	static const char* const names[] =
	{
		"Auto", "Hunt", "Flee", "Recover", "Wander"
	};
	int s = gCd2Cfg.aiForceState;

	if (s < 0 || s >= CD2_AI_STATE_COUNT)
		s = 0;

	if (s == CD2_AI_AUTO)
	{
		int cur = sState;

		if (cur < 0 || cur >= CD2_AI_STATE_COUNT)
			cur = 0;

		return names[cur];
	}

	return names[s];
}

void cd2AiRegister(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, cd2AiOnFrame, NULL, 1);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, cd2AiOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAR_STEP, cd2AiOnCarStep, NULL, 1);

	ctx->jer_log(ctx, "[combatd2] opponent AI registered (SDK v%d)\n", ctx->sdkVersion);
}
