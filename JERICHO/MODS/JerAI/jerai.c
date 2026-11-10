/*
 * jerai.c — module entry point for JerAI.
 *
 * Registers the module and its engine hooks, owns the shared state (gJerAi) and
 * the config refresh. The A* search library lives in jerai_map / jerai_star /
 * jerai_local, the road graph in jerai_road, the driver and behaviors in
 * jerai_drive / jerai_behavior, the debug drawing in jerai_viz and the pause
 * page in jerai_menu (see jerai_internal.h for the file map).
 *
 * WHAT IT SAYS, AND WHERE.
 *  - HUD: up to four short lines in the top-left corner (jer_hud_panel slots
 *    JERAI_HUD_SLOT0.., see the owner table in jer_hud.h), refreshed three times
 *    a second while `viz_hud` is on. Not the shared notice queue, so nothing
 *    else's messages are pushed out and nothing is cut short.
 *  - Log: quiet by default. A line when the drive's state changes (behavior,
 *    following, fleeing, chased, cop near, reversing out of a wedge). The
 *    35-field once-a-second drive line, the steering-loop line and the pad probe
 *    are written only while `steer_trace` is on.
 *
 * Driving is pad injection: the module produces a CAR_PAD_* bitmask in
 * JER_EVENT_CAR_PAD and the engine's own input path applies it, so the car stays
 * the player's own. See jerai_drive.h.
 *
 * Tunables live in JERICHO/CONFIG/JerAI.ini (jer_config namespace "JerAI",
 * see jerai.h) and are re-read live.
 */
#include "jerai_internal.h"
#include "jerai_drive.h"
#include "jerai_behavior.h"

#include "jer_config.h"
#include "jer_events.h"
#include "jer_hud.h"

#include "players.h"
#include "cars.h"
#include "pad.h"	/* CAR_PAD_* */
#include "felony.h"	/* GetPlayerFelony, pedestrianFelony */
#include "cop_ai.h"	/* copsAreInPursuit, CopsCanSeePlayer */
#include "dr2roads.h"	/* NumDriver2Straights/Curves/Junctions, for the graph log */

#if defined(_MSC_VER)
#include <intrin.h>	/* __rdtsc, for a per-run RNG seed */
#endif

#include <stdint.h>	/* uintptr_t, for the clock-less seed */
#include <stdio.h>
#include <string.h>

/* the single shared state */
JERAI_STATE gJerAi;

/* how often (frames) the tunables are re-read from the config cache */
#define JERAI_CONFIG_REFRESH 30

/* immunity helpers (defined below, used by the frame hook) */
static int JerAiImmuneActive(void);
static void JerAiClearWanted(void);

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

void JerAiRefreshConfig(void)
{
	gJerAi.enabled = jer_config_get_int("JerAI", JERAI_CFG_ENABLED, 1);
	gJerAi.mode = jer_config_get_int("JerAI", JERAI_CFG_MODE, 0);
	/* OFF by default: immunity zeroes the wanted level every frame, so with it on
	 * the police never chase and flee_police / Evade's run never fire */
	gJerAi.immune = jer_config_get_int("JerAI", JERAI_CFG_IMMUNE, 0);
	gJerAi.followEnabled = jer_config_get_int("JerAI", JERAI_CFG_FOLLOW, 1);
	gJerAi.followClearance = jer_config_get_int("JerAI", JERAI_CFG_FOLLOW_CLEARANCE, 1500);
	gJerAi.followCar = jer_config_get_int("JerAI", JERAI_CFG_FOLLOW_CAR, -1);
	gJerAi.followRange = jer_config_get_int("JerAI", JERAI_CFG_FOLLOW_RANGE, 0);
	gJerAi.fleeEnabled = jer_config_get_int("JerAI", JERAI_CFG_FLEE, 1);
	gJerAi.fleeCar = jer_config_get_int("JerAI", JERAI_CFG_FLEE_CAR, -1);
	gJerAi.fleeHost = jer_config_get_int("JerAI", JERAI_CFG_FLEE_HOST, 0);
	gJerAi.fleePolice = jer_config_get_int("JerAI", JERAI_CFG_FLEE_POLICE, 1);
	gJerAi.panicWheelspin = jer_config_get_int("JerAI", JERAI_CFG_PANIC_WHEELSPIN, 1);
	gJerAi.panicOffroad = jer_config_get_int("JerAI", JERAI_CFG_PANIC_OFFROAD, 30);
	gJerAi.panicLookAhead = jer_config_get_int("JerAI", JERAI_CFG_PANIC_LOOKAHEAD, 1600);
	gJerAi.yawLead = jer_config_get_int("JerAI", JERAI_CFG_YAW_LEAD, 4);
	gJerAi.steerTrace = jer_config_get_int("JerAI", JERAI_CFG_STEER_TRACE, 0);
	gJerAi.aimTest = jer_config_get_int("JerAI", JERAI_CFG_AIM_TEST, -1);
	gJerAi.pacePct = jer_config_get_int("JerAI", JERAI_CFG_PACE_PCT, 25);
	gJerAi.topSpeed = jer_config_get_int("JerAI", JERAI_CFG_TOP_SPEED, 255);
	gJerAi.runLights = jer_config_get_int("JerAI", JERAI_CFG_RUN_LIGHTS, 1);
	gJerAi.replanMs = jer_config_get_int("JerAI", JERAI_CFG_REPLAN_MS, 700);
	gJerAi.replanMove = jer_config_get_int("JerAI", JERAI_CFG_REPLAN_MOVE, 700);
	gJerAi.lookAhead = jer_config_get_int("JerAI", JERAI_CFG_LOOK_AHEAD, 1400);
	gJerAi.avoidDist = jer_config_get_int("JerAI", JERAI_CFG_AVOID_DIST, JERAI_AVOID_MAX);
	gJerAi.copClearance = jer_config_get_int("JerAI", JERAI_CFG_COP_CLEARANCE, 6000);
	gJerAi.maxSteer = jer_config_get_int("JerAI", JERAI_CFG_MAX_STEER, 512);
	gJerAi.cornerG = jer_config_get_int("JerAI", JERAI_CFG_CORNER_G, 2);
	gJerAi.brakeG = jer_config_get_int("JerAI", JERAI_CFG_BRAKE_GAIN, 1);
	gJerAi.cornerLookahead = jer_config_get_int("JerAI", JERAI_CFG_CORNER_LOOKAHEAD, 6000);
	gJerAi.speedScale = jer_config_get_int("JerAI", JERAI_CFG_SPEED_SCALE, 100);
	gJerAi.turnCost = jer_config_get_int("JerAI", JERAI_CFG_TURN_COST, 80);
	gJerAi.offroadCost = jer_config_get_int("JerAI", JERAI_CFG_OFFROAD_COST, JERAI_MAP_OFFROAD_DEFAULT);

	gJerAi.vizGrid = jer_config_get_int("JerAI", JERAI_CFG_VIZ_GRID, 0);
	gJerAi.vizRoad = jer_config_get_int("JerAI", JERAI_CFG_VIZ_ROAD, 1);
	gJerAi.vizPath = jer_config_get_int("JerAI", JERAI_CFG_VIZ_PATH, 1);
	gJerAi.vizHud = jer_config_get_int("JerAI", JERAI_CFG_VIZ_HUD, 1);

	/* a one-shot dump request: latch it and clear the key so it fires once */
	if (jer_config_get_int("JerAI", JERAI_CFG_DUMP_NOW, 0) != 0)
	{
		gJerAi.dumpRequested = 1;
		jer_config_set_int("JerAI", JERAI_CFG_DUMP_NOW, 0);
	}

	/* the same for forcing the menu open (a headless capture lever). Only with a
	 * level running: the pause it opens has nothing to stand on before that. */
	if (JerAiMapWorldLoaded() && jer_config_get_int("JerAI", JERAI_CFG_MENU_OPEN, 0) != 0)
	{
		JerAiMenuForceOpen();
		jer_config_set_int("JerAI", JERAI_CFG_MENU_OPEN, 0);
	}
}

void JerAiWriteConfigDefaults(void)
{
	/* Reading first means a value the player edited is written back unchanged;
	 * deliberately NOT re-saved later, or a live edit would be clobbered. */
	jer_config_set_int("JerAI", JERAI_CFG_ENABLED, gJerAi.enabled);
	jer_config_set_int("JerAI", JERAI_CFG_MODE, gJerAi.mode);
	jer_config_set_int("JerAI", JERAI_CFG_IMMUNE, gJerAi.immune);
	jer_config_set_int("JerAI", JERAI_CFG_FOLLOW, gJerAi.followEnabled);
	jer_config_set_int("JerAI", JERAI_CFG_FOLLOW_CLEARANCE, gJerAi.followClearance);
	jer_config_set_int("JerAI", JERAI_CFG_FOLLOW_CAR, gJerAi.followCar);
	jer_config_set_int("JerAI", JERAI_CFG_FOLLOW_RANGE, gJerAi.followRange);
	jer_config_set_int("JerAI", JERAI_CFG_FLEE, gJerAi.fleeEnabled);
	jer_config_set_int("JerAI", JERAI_CFG_FLEE_CAR, gJerAi.fleeCar);
	jer_config_set_int("JerAI", JERAI_CFG_FLEE_HOST, gJerAi.fleeHost);
	jer_config_set_int("JerAI", JERAI_CFG_FLEE_POLICE, gJerAi.fleePolice);
	jer_config_set_int("JerAI", JERAI_CFG_PANIC_WHEELSPIN, gJerAi.panicWheelspin);
	jer_config_set_int("JerAI", JERAI_CFG_PANIC_OFFROAD, gJerAi.panicOffroad);
	jer_config_set_int("JerAI", JERAI_CFG_PANIC_LOOKAHEAD, gJerAi.panicLookAhead);
	jer_config_set_int("JerAI", JERAI_CFG_YAW_LEAD, gJerAi.yawLead);
	jer_config_set_int("JerAI", JERAI_CFG_STEER_TRACE, gJerAi.steerTrace);
	jer_config_set_int("JerAI", JERAI_CFG_PACE_PCT, gJerAi.pacePct);
	jer_config_set_int("JerAI", JERAI_CFG_TOP_SPEED, gJerAi.topSpeed);
	jer_config_set_int("JerAI", JERAI_CFG_RUN_LIGHTS, gJerAi.runLights);
	jer_config_set_int("JerAI", JERAI_CFG_REPLAN_MS, gJerAi.replanMs);
	jer_config_set_int("JerAI", JERAI_CFG_REPLAN_MOVE, gJerAi.replanMove);
	jer_config_set_int("JerAI", JERAI_CFG_LOOK_AHEAD, gJerAi.lookAhead);
	jer_config_set_int("JerAI", JERAI_CFG_AVOID_DIST, gJerAi.avoidDist);
	jer_config_set_int("JerAI", JERAI_CFG_COP_CLEARANCE, gJerAi.copClearance);
	jer_config_set_int("JerAI", JERAI_CFG_MAX_STEER, gJerAi.maxSteer);
	jer_config_set_int("JerAI", JERAI_CFG_CORNER_G, gJerAi.cornerG);
	jer_config_set_int("JerAI", JERAI_CFG_BRAKE_GAIN, gJerAi.brakeG);
	jer_config_set_int("JerAI", JERAI_CFG_CORNER_LOOKAHEAD, gJerAi.cornerLookahead);
	jer_config_set_int("JerAI", JERAI_CFG_SPEED_SCALE, gJerAi.speedScale);
	jer_config_set_int("JerAI", JERAI_CFG_TURN_COST, gJerAi.turnCost);
	jer_config_set_int("JerAI", JERAI_CFG_OFFROAD_COST, gJerAi.offroadCost);
	jer_config_set_int("JerAI", JERAI_CFG_VIZ_GRID, gJerAi.vizGrid);
	jer_config_set_int("JerAI", JERAI_CFG_VIZ_ROAD, gJerAi.vizRoad);
	jer_config_set_int("JerAI", JERAI_CFG_VIZ_PATH, gJerAi.vizPath);
	jer_config_set_int("JerAI", JERAI_CFG_VIZ_HUD, gJerAi.vizHud);
}

/* ------------------------------------------------------------------ */
/* the engine road graph, built on demand                              */
/* ------------------------------------------------------------------ */

int JerAiEnsureRoadGraph(void)
{
	if (gJerAi.roadBuilt)
		return 1;

	if (!JerAiMapWorldLoaded())
		return 0;

	if (!JerAiRoadBuild(&gJerAi.road))
		return 0;

	gJerAi.roadBuilt = 1;

	/* the extractor's counts against what the engine says it holds: they must
	 * agree, or a surface kind is being misread */
	gJerAi.ctx->jer_log(gJerAi.ctx,
		"[JerAI] road graph: nodes=%d edges=%d (straights=%d/%d curves=%d/%d junctions=%d/%d)%s\n",
		gJerAi.road.nodeCount, gJerAi.road.edgeCount,
		gJerAi.road.straights, NumDriver2Straights,
		gJerAi.road.curves, NumDriver2Curves,
		gJerAi.road.junctions, NumDriver2Junctions,
		gJerAi.road.truncated ? " TRUNCATED" : "");

	return 1;
}

/* ------------------------------------------------------------------ */
/* hooks                                                               */
/* ------------------------------------------------------------------ */

/* CAR_PAD: the engine is about to turn the player's pad into pedals/steering.
 * For the live player's car, replace the pad with the active behavior's. */
static int JerAiOnCarPad(void* userdata, void* args)
{
	JER_ARGS_CAR_PAD* p = (JER_ARGS_CAR_PAD*)args;
	const JERAI_BEHAVIOR* b;
	CAR_DATA* car;
	unsigned int pad;

	(void)userdata;

	if (p == NULL || !gJerAi.enabled)
		return JER_RESULT_CONTINUE;

	if (!p->live)
		return JER_RESULT_CONTINUE;	/* only the live player's own car */

	car = JerAiOwnCar();

	if (car == NULL || (void*)car != p->car)
		return JER_RESULT_CONTINUE;

	/* The key that closed the settings page is still the page's until it is
	 * let go: without this, Cross on Close accelerated and Triangle left the car
	 * (or, as a handbrake edge, cycled the control mode) on the first frame back. */
	p->pad &= ~(int)JerAiMenuPadMask();

	b = JerAiBehaviorFor(gJerAi.mode);

	if (b == NULL)
		return JER_RESULT_CONTINUE;	/* Manual */

	/* the wall-evasion handbrake is asked for fresh by this frame's tick: a
	 * behavior that returns early (no plan / no aim) must not inherit the last
	 * frame's request and sit there held at thrust 0 */
	gJerAi.wantHandbrake = 0;

	pad = b->tick(car);

	p->pad = (int)pad;
	/* Feed the engine a DIGITAL pad: steering from CAR_PAD_LEFT/RIGHT is only
	 * read when use_analogue == 0 (handling.c). Leaving the physical pad's
	 * analogue flag set is why the car accelerated but never turned. */
	p->padSteer = 0;
	p->useAnalogue = 0;
	/* handled stays 0 for normal driving: the stock path turns the pad into
	 * pedals and steering, which is exactly the point - we feed it a synthetic
	 * pad. It is set to 1 only for the wall-evasion handbrake below. */

	/* Wall-evasion handbrake, applied through the HANDLED side door: setting the
	 * pad's HANDBRAKE bit would make handling.c cycle g_PlayerControlMode (its
	 * press edge does), handing the car to the engine's AI. Writing cp->handbrake
	 * with handled = 1 skips the stock block entirely, so the mode is untouched. */
	if (gJerAi.wantHandbrake)
	{
		p->handled = 1;
		car->handbrake = 1;
		car->thrust = 0;
		car->wheelspin = 0;
	}

	/* STEER TRACE (`steer_trace` = log every N frames, 0 = off). The once-a-second
	 * drive line cannot show a saw: it samples 30 frames apart while the wheel is
	 * turned on and off in ones and twos. This is the loop itself - the pad, the
	 * heading error, the DAMPED error the decision used, the feeler bias, the
	 * speed, the look-ahead the aim used, and the ACTUAL front wheel angle, which
	 * is where the engine's 64/frame centring shows up. */
	if (gJerAi.steerTrace > 0 && (gJerAi.tick % gJerAi.steerTrace) == 0)
	{
		gJerAi.ctx->jer_log(gJerAi.ctx,
			"[JerAI] steer t=%d pad=%x dir=%d diff=%d pred=%d bias=%d spd=%d tgt=%d wa=%d la=%d aimd=%d pdir=%d off=%d ax=%d az=%d gx=%d gz=%d wp=%d st=%d rec=%d hb=%d\n",
			gJerAi.tick, (int)pad, gJerAi.lastSteerDir, gJerAi.lastDiff, gJerAi.lastPred,
			gJerAi.lastBias, car->hd.speed, gJerAi.targetSpeed, gJerAi.lastWheel,
			gJerAi.aimLook, gJerAi.aimDist, gJerAi.aimPathDir, gJerAi.aimOff,
			gJerAi.aimX, gJerAi.aimZ,
			gJerAi.goalX, gJerAi.goalZ, gJerAi.path.waypoints,
			gJerAi.stuckFrames, gJerAi.recoverFrames,
			gJerAi.wantHandbrake);
	}

	/* one-shot probe: the pad the engine offered vs the one we return, so the
	 * bit meanings are never guessed at again */
	if (gJerAi.steerTrace > 0)
	{
		static int probed = 0;

		if (probed < 3)
		{
			probed++;
			gJerAi.ctx->jer_log(gJerAi.ctx,
				"[JerAI] pad probe: in=%x out=%x accel=%x brake=%x left=%x right=%x handbrake=%x\n",
				(unsigned int)p->pad, pad, (unsigned int)CAR_PAD_ACCEL, (unsigned int)CAR_PAD_BRAKE,
				(unsigned int)CAR_PAD_LEFT, (unsigned int)CAR_PAD_RIGHT, (unsigned int)CAR_PAD_HANDBRAKE);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* the state line                                                      */
/* ------------------------------------------------------------------ */

/* One log line whenever the drive's state changes, nothing in between. The
 * per-behavior flags are only meaningful while a behavior runs, so Manual (and a
 * disabled JerAI) reads as all-clear rather than whatever the last drive left. */
void JerAiLogState(const char* why)
{
	static int last[7] = { -2, -2, -2, -2, -2, -2, -2 };
	int driving = gJerAi.enabled && gJerAi.mode != JERAI_MODE_MANUAL;
	int now[7];
	int i, same = 1;

	now[0] = gJerAi.enabled;
	now[1] = gJerAi.mode;
	now[2] = driving && gJerAi.followActive;
	now[3] = driving && gJerAi.fleeActive;
	now[4] = driving && gJerAi.panicFlee;
	now[5] = driving && gJerAi.evadeActive;
	now[6] = driving && gJerAi.recoverFrames > 0;

	for (i = 0; i < 7; i++)
	{
		if (now[i] != last[i])
			same = 0;

		last[i] = now[i];
	}

	if (same)
		return;

	gJerAi.ctx->jer_log(gJerAi.ctx,
		"[JerAI] state (%s): %s %s follow=%d flee=%d chased=%d cop_near=%d reversing=%d\n",
		why != NULL ? why : "-", now[0] ? "on" : "off", JerAiBehaviorName(gJerAi.mode),
		now[2], now[3], now[4], now[5], now[6]);
}

/* ------------------------------------------------------------------ */
/* the HUD corner                                                      */
/* ------------------------------------------------------------------ */

/* jer_hud_panel slots, top left (owner table in jer_hud.h); mp's clock is 5 */
#define JERAI_HUD_SLOT0		6
#define JERAI_HUD_LINES		4
#define JERAI_HUD_REFRESH	10	/* frames between text refreshes: readable, not flickery */

/* a distance in world units as a short "850" / "4.2k" */
static void JerAiFmtDist(char* out, int max, long long d)
{
	if (d < 0)
		d = 0;

	if (d < 1000)
		snprintf(out, max, "%d", (int)d);
	else if (d < 100000)
		snprintf(out, max, "%d.%dk", (int)(d / 1000), (int)((d % 1000) / 100));
	else
		snprintf(out, max, "%dk", (int)(d / 1000));
}

static long long JerAiIsqrt(long long v)
{
	long long r = 0, b = (long long)1 << 62;

	if (v <= 0)
		return 0;

	while (b > v)
		b >>= 2;

	while (b != 0)
	{
		if (v >= r + b)
		{
			v -= r + b;
			r = (r >> 1) + b;
		}
		else
			r >>= 1;

		b >>= 2;
	}

	return r;
}

static void JerAiHudClear(void)
{
	int i;

	for (i = 0; i < JERAI_HUD_LINES; i++)
		jer_hud_panel_clear(JERAI_HUD_SLOT0 + i);
}

static void JerAiHudLine(int i, const char* text, int r, int g, int b)
{
	if (text != NULL && text[0] != '\0')
		jer_hud_panel(JERAI_HUD_SLOT0 + i, JER_HUD_ANCHOR_TOP_LEFT, text, r, g, b);
	else
		jer_hud_panel_clear(JERAI_HUD_SLOT0 + i);
}

/* Up to four short lines:
 *   1  JerAI <behavior> [what it is doing]   (colour = behavior, red when chased)
 *   2  speed and the speed it is aiming for
 *   3  the route: waypoints and distance to the goal
 *   4  the situation: nearest cop / followed car / the corner ahead */
void JerAiHudUpdate(void)
{
	static int shown;
	char line[JERAI_HUD_LINES][48];
	char d1[16], d2[16];
	CAR_DATA* car;
	int r = 230, g = 230, b = 230;
	int driving;

	if (!gJerAi.enabled || !gJerAi.vizHud)
	{
		if (shown)
			JerAiHudClear();

		shown = 0;
		return;
	}

	if (shown && (gJerAi.tick % JERAI_HUD_REFRESH) != 0)
		return;

	shown = 1;
	memset(line, 0, sizeof(line));
	car = JerAiOwnCar();
	driving = gJerAi.mode != JERAI_MODE_MANUAL && car != NULL;

	if (!driving)
	{
		JerAiHudLine(0, "JerAI Manual  (L2: settings)", 170, 170, 170);

		for (int i = 1; i < JERAI_HUD_LINES; i++)
			JerAiHudLine(i, NULL, 0, 0, 0);

		return;
	}

	/* line 1: what it is doing */
	{
		const char* doing = "";

		if (gJerAi.recoverFrames > 0)
			doing = " - reversing";
		else if (gJerAi.panicFlee)
			doing = " - CHASED";
		else if (gJerAi.evadeActive)
			doing = " - leaving cop";
		else if (gJerAi.fleeActive)
			doing = " - fleeing";
		else if (gJerAi.followActive)
			doing = " - following";
		else if (gJerAi.lightHold)
			doing = " - red light";

		snprintf(line[0], sizeof(line[0]), "JerAI %s%s", JerAiBehaviorName(gJerAi.mode), doing);

		if (gJerAi.panicFlee)
			r = 255, g = 90, b = 90;
		else if (gJerAi.mode == JERAI_MODE_EVADE)
			r = 255, g = 200, b = 80;
		else
			r = 120, g = 220, b = 255;
	}

	/* line 2: speed */
	if (gJerAi.speedCap > 0)
		snprintf(line[1], sizeof(line[1]), "Speed %d / %d  cap %d", car->hd.speed, gJerAi.targetSpeed, gJerAi.speedCap);
	else
		snprintf(line[1], sizeof(line[1]), "Speed %d / %d", car->hd.speed, gJerAi.targetSpeed);

	/* line 3: the route */
	{
		long long dx = (long long)gJerAi.goalX - car->hd.where.t[0];
		long long dz = (long long)gJerAi.goalZ - car->hd.where.t[2];

		JerAiFmtDist(d1, sizeof(d1), JerAiIsqrt(dx * dx + dz * dz));

		if (gJerAi.havePlan)
			snprintf(line[2], sizeof(line[2]), "Route %d pts%s, goal %s", gJerAi.path.waypoints,
				gJerAi.path.complete ? "" : " (part)", d1);
		else
			snprintf(line[2], sizeof(line[2]), "Route: planning");
	}

	/* line 4: the situation */
	if (gJerAi.mode == JERAI_MODE_EVADE)
	{
		if (gJerAi.evadeCopDist > 0)
		{
			JerAiFmtDist(d1, sizeof(d1), gJerAi.evadeCopDist);
			snprintf(line[3], sizeof(line[3]), "Cop %s  limit %d", d1, gJerAi.roadLimit);
		}
		else
			snprintf(line[3], sizeof(line[3]), "No cop near  limit %d", gJerAi.roadLimit);
	}
	else if (gJerAi.followActive || gJerAi.fleeActive)
	{
		JerAiFmtDist(d1, sizeof(d1), gJerAi.followDist);
		snprintf(line[3], sizeof(line[3]), "%s %s", gJerAi.fleeActive ? "Chaser" : "Target", d1);
	}
	else if (gJerAi.curve.sharp != 0)
	{
		/* heading change in game units (4096 = a full turn), shown in degrees */
		int deg = (gJerAi.curve.sharp < 0 ? -gJerAi.curve.sharp : gJerAi.curve.sharp) * 360 / 4096;

		JerAiFmtDist(d2, sizeof(d2), gJerAi.curve.dist);
		snprintf(line[3], sizeof(line[3]), "Corner %d deg in %s  grip %d%%", deg, d2, gJerAi.gripPct);
	}
	else
		snprintf(line[3], sizeof(line[3]), "Grip %d%%", gJerAi.gripPct);

	JerAiHudLine(0, line[0], r, g, b);
	JerAiHudLine(1, line[1], 230, 230, 230);
	JerAiHudLine(2, line[2], 230, 230, 230);
	JerAiHudLine(3, line[3], 230, 230, 230);
}

static int JerAiOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gJerAi.tick++;

	/* the menu reads the PHYSICAL pad, so it works while JerAI drives */
	JerAiMenuTick();

	/* immunity: keep the wanted level at zero so no cop sirens chase the drive
	 * (the damage half is JER_EVENT_GET_BUDDHA, below) */
	if (JerAiImmuneActive())
		JerAiClearWanted();

	if (++gJerAi.frame % JERAI_CONFIG_REFRESH == 0)
		JerAiRefreshConfig();

	/* the road graph is valid only while a level is loaded; drop it between
	 * levels so it is rebuilt for the next one */
	if (!JerAiMapWorldLoaded())
		gJerAi.roadBuilt = 0;

	/* the road-graph layer needs the graph: build it once per level when that
	 * layer is on (it used to be built only by a dump, so the layer - on by
	 * default - never drew). A failed build retries twice a second, not every
	 * frame. */
	if (gJerAi.enabled && gJerAi.vizRoad && !gJerAi.roadBuilt && (gJerAi.tick % 15) == 0)
		JerAiEnsureRoadGraph();

	/* a dump the menu or the config asked for. Handled before the enabled gate:
	 * asking for a graph is not a driving behavior. */
	if (gJerAi.dumpRequested)
	{
		if (JerAiEnsureRoadGraph() && JerAiDumpRoadGraph(&gJerAi.road))
			gJerAi.ctx->jer_log(gJerAi.ctx,
				"[JerAI] wrote JerAI_roadgraph.json/.obj (%d nodes, %d edges)\n",
				gJerAi.road.nodeCount, gJerAi.road.edgeCount);
		else
			gJerAi.ctx->jer_log(gJerAi.ctx, "[JerAI] dump requested but no road data yet\n");

		gJerAi.dumpRequested = 0;
	}

	/* the state line (on change only) and the HUD corner */
	JerAiLogState("drive");
	JerAiHudUpdate();

	/* STEER TRACE ONLY: once a second while driving, every number the drive
	 * uses, so a run is legible from the log (and a stalled car shows as an
	 * unchanging pos). Off by default - it is ~35 fields a second. */
	if (gJerAi.steerTrace > 0 && gJerAi.enabled && gJerAi.mode != JERAI_MODE_MANUAL && (gJerAi.tick % 30) == 0)
	{
		CAR_DATA* car = JerAiOwnCar();

		if (car != NULL)
		{
			const JERAI_BEHAVIOR* b = JerAiBehaviorFor(gJerAi.mode);

			gJerAi.ctx->jer_log(gJerAi.ctx,
				"[JerAI] drive mode=%s pos=(%d,%d) speed=%d dir=%d slip=%d yaw=%d tgt=%d grip=%d sharp=%d at=%d wp=%d complete=%d diff=%d bias=%d steer=%d hb=%d pad=%x goal=(%d,%d) aim=(%d,%d) la=%d aimd=%d stuck=%d rec=%d vizlines=%d follow=%d fdist=%d flee=%d police=%d evade=%d cop=%d limit=%d cap=%d light=%d\n",
				b != NULL ? b->name : "?",
				car->hd.where.t[0], car->hd.where.t[2], car->hd.speed, car->hd.direction,
				gJerAi.carState.slip, gJerAi.carState.yawRate,
				gJerAi.targetSpeed, gJerAi.gripPct,
				gJerAi.curve.sharp, gJerAi.curve.dist,
				gJerAi.path.waypoints, gJerAi.path.complete, gJerAi.lastDiff, gJerAi.lastBias,
				gJerAi.lastSteerDir, gJerAi.wantHandbrake, gJerAi.lastPad,
				gJerAi.goalX, gJerAi.goalZ, gJerAi.aimX, gJerAi.aimZ,
				gJerAi.aimLook, gJerAi.aimDist, gJerAi.stuckFrames, gJerAi.recoverFrames,
				JerAiVizLines(),
				gJerAi.followActive, gJerAi.followDist, gJerAi.fleeActive, gJerAi.policeActive,
				gJerAi.evadeActive, gJerAi.evadeCopDist, gJerAi.roadLimit, gJerAi.speedCap, gJerAi.lightHold);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* a per-run seed so the wander is not identical every launch (Random2 is
 * frame-deterministic). JER_EVENT_GAME_START carries the debug -seed. */
static unsigned int JerAiSeedFromClock(void)
{
#if defined(_MSC_VER)
	return (unsigned int)__rdtsc();
#else
	static int marker;
	/* uintptr_t, not unsigned long: a pointer does not fit a 32-bit long on
	 * Win64 (mingw/clang refuse the cast) */
	return (unsigned int)(uintptr_t)&marker;
#endif
}

static int JerAiOnGameStart(void* userdata, void* args)
{
	JER_ARGS_GAME_START* gs = (JER_ARGS_GAME_START*)args;
	unsigned int seed;

	(void)userdata;

	seed = (gs != NULL && gs->seed != 0) ? (unsigned int)gs->seed : JerAiSeedFromClock();

	JerAiRandSeed(seed);

	/* a new level: the plan, the goal and the stuck/feeler state all describe the
	 * last one (its coordinates, its heading), so start clean. The road graph
	 * belongs to the level too. */
	JerAiDriveResetRun();
	JerAiBehaviorResetRun();
	gJerAi.roadBuilt = 0;
	JerAiHudClear();

	gJerAi.ctx->jer_log(gJerAi.ctx, "[JerAI] run seed = %u\n", seed);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* immunity                                                            */
/* ------------------------------------------------------------------ */

/* While JerAI is driving, the car is invulnerable and the wanted level stays at
 * zero: no damage totals it (see JER_EVENT_GET_BUDDHA) and no cop sirens chase
 * the test drive. Gated on an active, enabled drive so Manual play is normal. */
static int JerAiImmuneActive(void)
{
	return gJerAi.enabled && gJerAi.immune && gJerAi.mode != JERAI_MODE_MANUAL;
}

static int JerAiOnGetBuddha(void* userdata, void* args)
{
	JER_ARGS_QUERY_FLAG* q = (JER_ARGS_QUERY_FLAG*)args;

	(void)userdata;

	if (q != NULL && JerAiImmuneActive())
		q->result = 1;

	return JER_RESULT_CONTINUE;
}

static void JerAiClearWanted(void)
{
	int id = player[0].playerCarId;

	if (id >= 0 && id < MAX_CARS)
		car_data[id].felonyRating = 0;

	copsAreInPursuit = 0;
	CopsCanSeePlayer = 0;
	pedestrianFelony = 0;
}

/* ------------------------------------------------------------------ */
/* module entry                                                        */
/* ------------------------------------------------------------------ */

JER_MODULE_ENTRY(jer_module_JerAI_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&gJerAi, 0, sizeof(gJerAi));
	gJerAi.ctx = ctx;
	gJerAi.roadLimit = 56;		/* civ_ai.c speedLimits[0], until Evade reads the road */
	gJerAi.mapBuiltAt = -1;
	gJerAi.prevDir = -1;	/* no last heading yet: the first frame has no yaw */
	JerAiRandSeed(0x12345678u);

	ctx->jer_register_module(ctx,
		"JerAI",			/* id */
		"JerAI",			/* name */
		"0.3.0",			/* version (= mod.toml) */
		"Jaret Ludvik",			/* author */
		"General-purpose AI driver for the player's car: A* pathfinding, road-graph extraction, joyride/random-wander and cop-evade behaviors, debug visualization and a pause-menu settings page.",	/* description */
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	JerAiRefreshConfig();
	JerAiWriteConfigDefaults();
	JerAiMenuRegister();

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, JerAiOnCarPad, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, JerAiOnFrame, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, JerAiOnGameStart, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_BUDDHA, JerAiOnGetBuddha, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, JerAiOnDrawWorld, &gJerAi, 0);

	ctx->jer_log(ctx, "[JerAI] registered - enabled=%d mode=%d (%s) immune=%d pace=%d pct\n",
		gJerAi.enabled, gJerAi.mode, JerAiBehaviorName(gJerAi.mode), gJerAi.immune, gJerAi.pacePct);
}
