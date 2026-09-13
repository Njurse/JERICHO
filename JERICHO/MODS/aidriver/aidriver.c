/*
 * aidriver.c — general-purpose AI driver for the player's car.
 *
 * When the sandbox (or anything else) hands the player's car to one of the
 * engine AI drivers (g_PlayerControlMode != 0 -> controlType is
 * CIV_AI / PURSUER_AI / LEAD_AI), this module makes that driver behave the
 * way a player would:
 *
 *   - joyride   : free-flowing drive around the map — never parks, never
 *                 stops at lights/yields (hard obstacles still brake), at a
 *                 pace a quarter above the road's traffic limit;
 *   - evade     : when the cops are a real threat (felony > pursuit
 *                 threshold, copsAreInPursuit, or a cop inside
 *                 cop_clearance) — full speed, runs lights, no parking;
 *   - pursuit   : in a chase mission (Mission.ChaseTarget valid) — tails
 *                 the target car, holding pursuit_dist behind it.
 *
 * The behaviors apply to all three AI control types where the engine
 * exposes the relevant knobs: CIV_AI gets maxSpeed/thrustState/ctrlState
 * overrides, PURSUER_AI gets desiredSpeed, LEAD_AI drives its own route
 * (road-follower — left untouched).
 *
 * Timing: the PRE_SIM hook runs at the top of StepSim, BEFORE the AI
 * control loop reads the civ state, so maxSpeed / stop-state overrides are
 * honored the same frame. The FRAME hook runs inside GlobalTimeStep after
 * the AI computed thrust/steer but before StepCars integrates them, so
 * direct thrust/wheel_angle overrides (pursuit tailing, un-sticking a stop
 * the AI re-set mid-frame) move the car the same frame.
 *
 * Tunables live in JERICHO/CONFIG/aidriver.ini (jer_config namespace
 * "aidriver", see aidriver.h) and are re-read live.
 */
#include "jericho.h"
#include "jer_config.h"
#include "jer_events.h"

#include "driver2.h"
#include "cars.h"
#include "players.h"
#include "felony.h"
#include "cop_ai.h"
#include "mission.h"
#include "overlay.h"
#include "civ_ai.h"
#include "dr2math.h"

#include "aidriver.h"

#include <math.h>
#include <string.h>

/* civ-AI acceleration constant (civ_ai.c: newAccel = 2000) */
#define AI_FULL_ACCEL 2000
#define AI_FULL_BRAKE (AI_FULL_ACCEL * 2)

/* civ-AI speed limits (civ_ai.c speedLimits[] — recaptured when the engine
 * re-inits the civ state and rewrites maxSpeed to the road limit) */
#define AI_ROAD_LIMIT_0 ((int)speedLimits[0])
#define AI_ROAD_LIMIT_1 ((int)speedLimits[1])
#define AI_ROAD_LIMIT_2 ((int)speedLimits[2])

/* how often (frames) the tunables are re-read from the config cache */
#define AI_CONFIG_REFRESH 30

typedef struct AIDRIVER_STATE
{
	JERICHO_CONTEXT* ctx;
	int frame;

	/* tunables (re-read from aidriver.ini) */
	int enabled;
	int pacePct;
	int topSpeed;
	int runLights;
	int pursuitDist;
	int pursuitMargin;
	int copClearance;
	int maxSteer;

	/* recaptured road limit for the pace boost (engine resets maxSpeed to
	 * the road limit on every civ-state re-init) */
	int roadLimit;
} AIDRIVER_STATE;

static AIDRIVER_STATE gState;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* the player's car, when it is currently AI-driven */
static CAR_DATA* AiPlayerCar(void)
{
	if (player[0].playerCarId < 0)
		return NULL;

	CAR_DATA* pc = &car_data[player[0].playerCarId];

	switch (pc->controlType)
	{
	case CONTROL_TYPE_CIV_AI:
	case CONTROL_TYPE_PURSUER_AI:
	case CONTROL_TYPE_LEAD_AI:
		return pc;
	default:
		return NULL;
	}
}

/* distance in world units to the nearest cop car (0 if none) */
static int AiNearestCop(CAR_DATA* pc)
{
	int best = 0x7fffffff;
	int i;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp == pc || cp->controlType != CONTROL_TYPE_PURSUER_AI)
			continue;

		int dx = cp->hd.where.t[0] - pc->hd.where.t[0];
		int dz = cp->hd.where.t[2] - pc->hd.where.t[2];
		double dsq = (double)((long long)dx * dx + (long long)dz * dz);
		int dist = (int)sqrt(dsq);

		if (dist < best)
			best = dist;
	}

	return best == 0x7fffffff ? 0 : best;
}

/* heading in game units (0..4095, +Z = 0, +X = 1024) toward (dx, dz) —
 * matches the engine's ratan2(dx, dz) convention used by CivSteerAngle */
static int AiHeadingTo(int dx, int dz)
{
	double a = atan2((double)dx, (double)dz);	/* C convention */
	int ang = (int)(a * 2048.0 / 3.14159265358979323846);

	return (ang + 4096) & 0xfff;
}

/* is the civ AI currently trying to stop/park? */
static int AiCivWantsToStop(CAR_DATA* pc)
{
	switch (pc->ai.c.ctrlState)
	{
	case CIV_AI_CTRL_PARKED:
	case CIV_AI_CTRL_WAIT_TRAFFIC_LIGHT:
	case CIV_AI_CTRL_YIELD_TURN:
	case CIV_AI_CTRL_STOP_AT_NODE:
		return 1;
	default:
		break;
	}

	return (pc->ai.c.thrustState == CIV_AI_THRUST_STOP ||
		pc->ai.c.thrustState == CIV_AI_THRUST_STOP_AT_NODE);
}

/* force the civ AI back into "keep moving" so joyride/evade never stops
 * at lights, yields or parking spots */
static void AiCivUnstick(CAR_DATA* pc)
{
	pc->ai.c.ctrlState = CIV_AI_CTRL_DRIVE;
	pc->ai.c.thrustState = CIV_AI_THRUST_ACCELERATE;
	pc->ai.c.ctrlNode = NULL;
}

/* are the cops a real threat right now? Mode-aware: in a getaway every
 * cop is the point of the mode; otherwise a cop inside cop_clearance or
 * an active pursuit counts. */
static int AiThreatened(CAR_DATA* pc, int felony, int copDist)
{
	if (GameType == GAME_GETAWAY)
		return 1;

	if (felony > FELONY_PURSUIT_MIN_VALUE || copsAreInPursuit != 0)
		return 1;

	if (gState.copClearance > 0 && copDist != 0 && copDist < gState.copClearance)
		return 1;

	(void)pc;
	return 0;
}

/* ------------------------------------------------------------------ */
/* PRE_SIM: state overrides the AI reads this frame                    */
/* ------------------------------------------------------------------ */

static int AiDriverPreSim(void* userdata, void* args)
{
	CAR_DATA* pc;
	int felony;
	int copDist;
	int evade;
	int i;

	(void)userdata;
	(void)args;

	if (++gState.frame % AI_CONFIG_REFRESH == 0)
	{
		gState.enabled = jer_config_get_int("aidriver", AIDRIVER_CFG_ENABLED, 1);
		gState.pacePct = jer_config_get_int("aidriver", AIDRIVER_CFG_PACE_PCT, 25);
		gState.topSpeed = jer_config_get_int("aidriver", AIDRIVER_CFG_TOP_SPEED, 255);
		gState.runLights = jer_config_get_int("aidriver", AIDRIVER_CFG_RUN_LIGHTS, 1);
		gState.pursuitDist = jer_config_get_int("aidriver", AIDRIVER_CFG_PURSUIT_DIST, 4000);
		gState.pursuitMargin = jer_config_get_int("aidriver", AIDRIVER_CFG_PURSUIT_MARGIN, 800);
		gState.copClearance = jer_config_get_int("aidriver", AIDRIVER_CFG_COP_CLEARANCE, 6000);
		gState.maxSteer = jer_config_get_int("aidriver", AIDRIVER_CFG_MAX_STEER, 512);
	}

	if (!gState.enabled || g_PlayerControlMode == 0)
		return JER_RESULT_CONTINUE;

	pc = AiPlayerCar();

	if (pc == NULL)
		return JER_RESULT_CONTINUE;

	felony = *GetPlayerFelony(&player[0]);
	copDist = AiNearestCop(pc);
	evade = AiThreatened(pc, felony, copDist);

	if (pc->controlType == CONTROL_TYPE_CIV_AI)
	{
		int limit = pc->ai.c.maxSpeed;

		/* recapture the road limit when the engine re-inited the civ state
		 * and rewrote maxSpeed to the road's limit — never compounds */
		if (limit == AI_ROAD_LIMIT_0 || limit == AI_ROAD_LIMIT_1 || limit == AI_ROAD_LIMIT_2)
			gState.roadLimit = limit;

		/* evade: as fast as the car can go; joyride: a confident pace above
		 * the traffic */
		pc->ai.c.maxSpeed = evade ? gState.topSpeed
					  : gState.roadLimit + gState.roadLimit * gState.pacePct / 100;

		if (pc->ai.c.maxSpeed > 255)
			pc->ai.c.maxSpeed = 255;
		else if (pc->ai.c.maxSpeed < 0)
			pc->ai.c.maxSpeed = 0;

		/* free-flowing / evasion: never park or stop at lights */
		if (evade || gState.runLights)
		{
			if (AiCivWantsToStop(pc))
				AiCivUnstick(pc);
		}
	}
	else if (pc->controlType == CONTROL_TYPE_PURSUER_AI)
	{
		/* cop AI: its own power/speed already scale with the felony; just
		 * pin the throttle wide open when threatened */
		if (evade)
			pc->ai.p.desiredSpeed = 2047;
	}

	/* LEAD_AI (FreeRoamer) follows its own route at mission pace — no
	 * engine knob to force speed, so it is left to drive */

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* FRAME: direct overrides after the AI ran, before physics            */
/* ------------------------------------------------------------------ */

static int AiDriverOnFrame(void* userdata, void* args)
{
	CAR_DATA* pc;
	int felony;
	int copDist;
	int evade;

	(void)userdata;
	(void)args;

	if (!gState.enabled || g_PlayerControlMode == 0)
		return JER_RESULT_CONTINUE;

	pc = AiPlayerCar();

	if (pc == NULL)
		return JER_RESULT_CONTINUE;

	felony = *GetPlayerFelony(&player[0]);
	copDist = AiNearestCop(pc);
	evade = AiThreatened(pc, felony, copDist);

	/* 1) the civ AI may have re-set a stop state (or brake thrust) after
	 * PRE_SIM — un-stick it and keep the car rolling */
	if (pc->controlType == CONTROL_TYPE_CIV_AI && (evade || gState.runLights))
	{
		if (AiCivWantsToStop(pc))
		{
			AiCivUnstick(pc);

			if (pc->thrust <= 0)
				pc->thrust = AI_FULL_ACCEL;
		}
	}

	/* 2) pursuit tailing: chase the mission target, holding pursuit_dist */
	if (Mission.ChaseTarget != NULL)
	{
		int slot = Mission.ChaseTarget->s.car.slot;

		if (slot >= 0 && slot < MAX_CARS)
		{
			CAR_DATA* tgt = &car_data[slot];

			if (tgt->controlType != CONTROL_TYPE_NONE)
			{
				int dx = tgt->hd.where.t[0] - pc->hd.where.t[0];
				int dz = tgt->hd.where.t[2] - pc->hd.where.t[2];
				double dsq = (double)((long long)dx * dx + (long long)dz * dz);
				int dist = (int)sqrt(dsq);
				int steer = DIFF_ANGLES(pc->hd.direction, AiHeadingTo(dx, dz));
				int err = dist - gState.pursuitDist;

				if (steer > gState.maxSteer)
					steer = gState.maxSteer;
				else if (steer < -gState.maxSteer)
					steer = -gState.maxSteer;

				pc->wheel_angle = steer;

				if (err > gState.pursuitMargin)
					pc->thrust = AI_FULL_ACCEL;
				else if (err < -gState.pursuitMargin && pc->hd.wheel_speed > 0)
					pc->thrust = -AI_FULL_ACCEL;	/* brake, never reverse */
				else
					pc->thrust = 0;
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* module entry                                                        */
/* ------------------------------------------------------------------ */

JER_MODULE_ENTRY(jer_module_aidriver_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&gState, 0, sizeof(gState));
	gState.ctx = ctx;
	gState.roadLimit = 56;

	ctx->jer_register_module(ctx,
		"aidriver",			/* id */
		"AI Driver",			/* name */
		"0.1.0",			/* version */
		"Jaret Ludvik",			/* author */
		"General-purpose AI driver: free-flowing joyride, aggressive police evasion, pursuit tailing (tunable distances).",	/* description */
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	ctx->jer_register_hook(ctx, JER_EVENT_PRE_SIM, AiDriverPreSim, &gState, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, AiDriverOnFrame, &gState, 0);

	gState.enabled = jer_config_get_int("aidriver", AIDRIVER_CFG_ENABLED, 1);
	gState.pacePct = jer_config_get_int("aidriver", AIDRIVER_CFG_PACE_PCT, 25);
	gState.topSpeed = jer_config_get_int("aidriver", AIDRIVER_CFG_TOP_SPEED, 255);
	gState.runLights = jer_config_get_int("aidriver", AIDRIVER_CFG_RUN_LIGHTS, 1);
	gState.pursuitDist = jer_config_get_int("aidriver", AIDRIVER_CFG_PURSUIT_DIST, 4000);
	gState.pursuitMargin = jer_config_get_int("aidriver", AIDRIVER_CFG_PURSUIT_MARGIN, 800);
	gState.copClearance = jer_config_get_int("aidriver", AIDRIVER_CFG_COP_CLEARANCE, 6000);
	gState.maxSteer = jer_config_get_int("aidriver", AIDRIVER_CFG_MAX_STEER, 512);

	ctx->jer_log(ctx, "[aidriver] registered — enabled=%d pace=%d%% top=%d run_lights=%d pursuit_dist=%d cop_clearance=%d\n",
		gState.enabled, gState.pacePct, gState.topSpeed, gState.runLights,
		gState.pursuitDist, gState.copClearance);
}
