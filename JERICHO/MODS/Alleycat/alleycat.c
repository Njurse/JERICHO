/*
 * alleycat.c — module entry point for Alleycat.
 *
 * Registers the module and its engine hooks, owns the shared state (gAlleycat) and
 * the config refresh. The A* search library lives in alleycat_map / alleycat_star /
 * alleycat_local, the road graph in alleycat_road, the driver and behaviors in
 * alleycat_drive / alleycat_behavior, the debug drawing in alleycat_viz and the pause
 * page in alleycat_menu (see alleycat_internal.h for the file map).
 *
 * WHAT IT SAYS, AND WHERE.
 *  - HUD: up to four short lines in the top-left corner (jer_hud_panel slots
 *    ALLEYCAT_HUD_SLOT0.., see the owner table in jer_hud.h), refreshed three times
 *    a second while `viz_hud` is on. Not the shared notice queue, so nothing
 *    else's messages are pushed out and nothing is cut short.
 *  - Log: quiet by default. A line when the drive's state changes (behavior,
 *    following, fleeing, chased, cop near, reversing out of a wedge). The
 *    35-field once-a-second drive line, the steering-loop line and the pad probe
 *    are written only while `steer_trace` is on.
 *
 * Driving is pad injection: the module produces a CAR_PAD_* bitmask in
 * JER_EVENT_CAR_PAD and the engine's own input path applies it, so the car stays
 * the player's own. See alleycat_drive.h.
 *
 * Tunables live in JERICHO/CONFIG/Alleycat.ini (jer_config namespace "Alleycat",
 * see alleycat.h) and are re-read live.
 */
#include "alleycat_internal.h"
#include "alleycat_drive.h"
#include "alleycat_behavior.h"

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
ALLEYCAT_STATE gAlleycat;

/* how often (frames) the tunables are re-read from the config cache */
#define ALLEYCAT_CONFIG_REFRESH 30

/* immunity helpers (defined below, used by the frame hook) */
static int AlleycatImmuneActive(void);
static void AlleycatClearWanted(void);

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

void AlleycatRefreshConfig(void)
{
	gAlleycat.enabled = jer_config_get_int("Alleycat", ALLEYCAT_CFG_ENABLED, 1);
	gAlleycat.mode = jer_config_get_int("Alleycat", ALLEYCAT_CFG_MODE, 0);
	/* OFF by default: immunity zeroes the wanted level every frame, so with it on
	 * the police never chase and flee_police / Evade's run never fire */
	gAlleycat.immune = jer_config_get_int("Alleycat", ALLEYCAT_CFG_IMMUNE, 0);
	gAlleycat.followEnabled = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FOLLOW, 1);
	gAlleycat.followClearance = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FOLLOW_CLEARANCE, 1500);
	gAlleycat.followCar = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FOLLOW_CAR, -1);
	gAlleycat.followRange = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FOLLOW_RANGE, 0);
	gAlleycat.pursuitRam = jer_config_get_int("Alleycat", ALLEYCAT_CFG_PURSUIT_RAM, 0);
	gAlleycat.fleeEnabled = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FLEE, 1);
	gAlleycat.fleeCar = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FLEE_CAR, -1);
	gAlleycat.fleeHost = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FLEE_HOST, 0);
	gAlleycat.fleePolice = jer_config_get_int("Alleycat", ALLEYCAT_CFG_FLEE_POLICE, 1);
	gAlleycat.panicWheelspin = jer_config_get_int("Alleycat", ALLEYCAT_CFG_PANIC_WHEELSPIN, 1);
	gAlleycat.panicOffroad = jer_config_get_int("Alleycat", ALLEYCAT_CFG_PANIC_OFFROAD, 30);
	gAlleycat.panicLookAhead = jer_config_get_int("Alleycat", ALLEYCAT_CFG_PANIC_LOOKAHEAD, 1600);
	gAlleycat.yawLead = jer_config_get_int("Alleycat", ALLEYCAT_CFG_YAW_LEAD, 4);
	gAlleycat.steerTrace = jer_config_get_int("Alleycat", ALLEYCAT_CFG_STEER_TRACE, 0);
	gAlleycat.aimTest = jer_config_get_int("Alleycat", ALLEYCAT_CFG_AIM_TEST, -1);
	gAlleycat.pacePct = jer_config_get_int("Alleycat", ALLEYCAT_CFG_PACE_PCT, 25);
	gAlleycat.topSpeed = jer_config_get_int("Alleycat", ALLEYCAT_CFG_TOP_SPEED, 255);
	gAlleycat.runLights = jer_config_get_int("Alleycat", ALLEYCAT_CFG_RUN_LIGHTS, 1);
	gAlleycat.replanMs = jer_config_get_int("Alleycat", ALLEYCAT_CFG_REPLAN_MS, 700);
	gAlleycat.replanMove = jer_config_get_int("Alleycat", ALLEYCAT_CFG_REPLAN_MOVE, 700);
	gAlleycat.lookAhead = jer_config_get_int("Alleycat", ALLEYCAT_CFG_LOOK_AHEAD, 1400);
	gAlleycat.avoidDist = jer_config_get_int("Alleycat", ALLEYCAT_CFG_AVOID_DIST, ALLEYCAT_AVOID_MAX);
	gAlleycat.copClearance = jer_config_get_int("Alleycat", ALLEYCAT_CFG_COP_CLEARANCE, 6000);
	gAlleycat.maxSteer = jer_config_get_int("Alleycat", ALLEYCAT_CFG_MAX_STEER, 512);
	gAlleycat.cornerG = jer_config_get_int("Alleycat", ALLEYCAT_CFG_CORNER_G, 2);
	gAlleycat.brakeG = jer_config_get_int("Alleycat", ALLEYCAT_CFG_BRAKE_GAIN, 1);
	gAlleycat.cornerLookahead = jer_config_get_int("Alleycat", ALLEYCAT_CFG_CORNER_LOOKAHEAD, 6000);
	gAlleycat.speedScale = jer_config_get_int("Alleycat", ALLEYCAT_CFG_SPEED_SCALE, 100);
	gAlleycat.turnCost = jer_config_get_int("Alleycat", ALLEYCAT_CFG_TURN_COST, 80);
	gAlleycat.offroadCost = jer_config_get_int("Alleycat", ALLEYCAT_CFG_OFFROAD_COST, ALLEYCAT_MAP_OFFROAD_DEFAULT);

	gAlleycat.vizGrid = jer_config_get_int("Alleycat", ALLEYCAT_CFG_VIZ_GRID, 0);
	gAlleycat.vizRoad = jer_config_get_int("Alleycat", ALLEYCAT_CFG_VIZ_ROAD, 1);
	gAlleycat.vizPath = jer_config_get_int("Alleycat", ALLEYCAT_CFG_VIZ_PATH, 1);
	gAlleycat.vizHud = jer_config_get_int("Alleycat", ALLEYCAT_CFG_VIZ_HUD, 1);

	/* a one-shot dump request: latch it and clear the key so it fires once */
	if (jer_config_get_int("Alleycat", ALLEYCAT_CFG_DUMP_NOW, 0) != 0)
	{
		gAlleycat.dumpRequested = 1;
		jer_config_set_int("Alleycat", ALLEYCAT_CFG_DUMP_NOW, 0);
	}

	/* the same for forcing the menu open (a headless capture lever). Only with a
	 * level running: the pause it opens has nothing to stand on before that. */
	if (AlleycatMapWorldLoaded() && jer_config_get_int("Alleycat", ALLEYCAT_CFG_MENU_OPEN, 0) != 0)
	{
		AlleycatMenuForceOpen();
		jer_config_set_int("Alleycat", ALLEYCAT_CFG_MENU_OPEN, 0);
	}
}

void AlleycatWriteConfigDefaults(void)
{
	/* Reading first means a value the player edited is written back unchanged;
	 * deliberately NOT re-saved later, or a live edit would be clobbered. */
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_ENABLED, gAlleycat.enabled);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_MODE, gAlleycat.mode);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_IMMUNE, gAlleycat.immune);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FOLLOW, gAlleycat.followEnabled);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FOLLOW_CLEARANCE, gAlleycat.followClearance);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FOLLOW_CAR, gAlleycat.followCar);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FOLLOW_RANGE, gAlleycat.followRange);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_PURSUIT_RAM, gAlleycat.pursuitRam);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FLEE, gAlleycat.fleeEnabled);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FLEE_CAR, gAlleycat.fleeCar);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FLEE_HOST, gAlleycat.fleeHost);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_FLEE_POLICE, gAlleycat.fleePolice);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_PANIC_WHEELSPIN, gAlleycat.panicWheelspin);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_PANIC_OFFROAD, gAlleycat.panicOffroad);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_PANIC_LOOKAHEAD, gAlleycat.panicLookAhead);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_YAW_LEAD, gAlleycat.yawLead);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_STEER_TRACE, gAlleycat.steerTrace);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_PACE_PCT, gAlleycat.pacePct);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_TOP_SPEED, gAlleycat.topSpeed);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_RUN_LIGHTS, gAlleycat.runLights);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_REPLAN_MS, gAlleycat.replanMs);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_REPLAN_MOVE, gAlleycat.replanMove);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_LOOK_AHEAD, gAlleycat.lookAhead);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_AVOID_DIST, gAlleycat.avoidDist);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_COP_CLEARANCE, gAlleycat.copClearance);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_MAX_STEER, gAlleycat.maxSteer);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_CORNER_G, gAlleycat.cornerG);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_BRAKE_GAIN, gAlleycat.brakeG);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_CORNER_LOOKAHEAD, gAlleycat.cornerLookahead);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_SPEED_SCALE, gAlleycat.speedScale);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_TURN_COST, gAlleycat.turnCost);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_OFFROAD_COST, gAlleycat.offroadCost);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_VIZ_GRID, gAlleycat.vizGrid);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_VIZ_ROAD, gAlleycat.vizRoad);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_VIZ_PATH, gAlleycat.vizPath);
	jer_config_set_int("Alleycat", ALLEYCAT_CFG_VIZ_HUD, gAlleycat.vizHud);
}

/* ------------------------------------------------------------------ */
/* the engine road graph, built on demand                              */
/* ------------------------------------------------------------------ */

int AlleycatEnsureRoadGraph(void)
{
	if (gAlleycat.roadBuilt)
		return 1;

	if (!AlleycatMapWorldLoaded())
		return 0;

	if (!AlleycatRoadBuild(&gAlleycat.road))
		return 0;

	gAlleycat.roadBuilt = 1;

	/* the extractor's counts against what the engine says it holds: they must
	 * agree, or a surface kind is being misread */
	gAlleycat.ctx->jer_log(gAlleycat.ctx,
		"[Alleycat] road graph: nodes=%d edges=%d (straights=%d/%d curves=%d/%d junctions=%d/%d)%s\n",
		gAlleycat.road.nodeCount, gAlleycat.road.edgeCount,
		gAlleycat.road.straights, NumDriver2Straights,
		gAlleycat.road.curves, NumDriver2Curves,
		gAlleycat.road.junctions, NumDriver2Junctions,
		gAlleycat.road.truncated ? " TRUNCATED" : "");

	return 1;
}

/* ------------------------------------------------------------------ */
/* hooks                                                               */
/* ------------------------------------------------------------------ */

/* CAR_PAD: the engine is about to turn the player's pad into pedals/steering.
 * For the live player's car, replace the pad with the active behavior's. */
static int AlleycatOnCarPad(void* userdata, void* args)
{
	JER_ARGS_CAR_PAD* p = (JER_ARGS_CAR_PAD*)args;
	const ALLEYCAT_BEHAVIOR* b;
	CAR_DATA* car;
	unsigned int pad;

	(void)userdata;

	if (p == NULL || !gAlleycat.enabled)
		return JER_RESULT_CONTINUE;

	if (!p->live)
		return JER_RESULT_CONTINUE;	/* only the live player's own car */

	car = AlleycatOwnCar();

	if (car == NULL || (void*)car != p->car)
		return JER_RESULT_CONTINUE;

	/* The key that closed the settings page is still the page's until it is
	 * let go: without this, Cross on Close accelerated and Triangle left the car
	 * (or, as a handbrake edge, cycled the control mode) on the first frame back. */
	p->pad &= ~(int)AlleycatMenuPadMask();

	b = AlleycatBehaviorFor(gAlleycat.mode);

	if (b == NULL)
		return JER_RESULT_CONTINUE;	/* Manual */

	/* the wall-evasion handbrake is asked for fresh by this frame's tick: a
	 * behavior that returns early (no plan / no aim) must not inherit the last
	 * frame's request and sit there held at thrust 0 */
	gAlleycat.wantHandbrake = 0;

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
	if (gAlleycat.wantHandbrake)
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
	if (gAlleycat.steerTrace > 0 && (gAlleycat.tick % gAlleycat.steerTrace) == 0)
	{
		gAlleycat.ctx->jer_log(gAlleycat.ctx,
			"[Alleycat] steer t=%d pad=%x dir=%d diff=%d pred=%d bias=%d spd=%d tgt=%d wa=%d la=%d aimd=%d pdir=%d off=%d ax=%d az=%d gx=%d gz=%d wp=%d st=%d rec=%d hb=%d\n",
			gAlleycat.tick, (int)pad, gAlleycat.lastSteerDir, gAlleycat.lastDiff, gAlleycat.lastPred,
			gAlleycat.lastBias, car->hd.speed, gAlleycat.targetSpeed, gAlleycat.lastWheel,
			gAlleycat.aimLook, gAlleycat.aimDist, gAlleycat.aimPathDir, gAlleycat.aimOff,
			gAlleycat.aimX, gAlleycat.aimZ,
			gAlleycat.goalX, gAlleycat.goalZ, gAlleycat.path.waypoints,
			gAlleycat.stuckFrames, gAlleycat.recoverFrames,
			gAlleycat.wantHandbrake);
	}

	/* one-shot probe: the pad the engine offered vs the one we return, so the
	 * bit meanings are never guessed at again */
	if (gAlleycat.steerTrace > 0)
	{
		static int probed = 0;

		if (probed < 3)
		{
			probed++;
			gAlleycat.ctx->jer_log(gAlleycat.ctx,
				"[Alleycat] pad probe: in=%x out=%x accel=%x brake=%x left=%x right=%x handbrake=%x\n",
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
 * disabled Alleycat) reads as all-clear rather than whatever the last drive left. */
void AlleycatLogState(const char* why)
{
	static int last[7] = { -2, -2, -2, -2, -2, -2, -2 };
	int driving = gAlleycat.enabled && gAlleycat.mode != ALLEYCAT_MODE_MANUAL;
	int now[7];
	int i, same = 1;

	now[0] = gAlleycat.enabled;
	now[1] = gAlleycat.mode;
	now[2] = driving && gAlleycat.followActive;
	now[3] = driving && gAlleycat.fleeActive;
	now[4] = driving && gAlleycat.panicFlee;
	now[5] = driving && gAlleycat.evadeActive;
	now[6] = driving && gAlleycat.recoverFrames > 0;

	for (i = 0; i < 7; i++)
	{
		if (now[i] != last[i])
			same = 0;

		last[i] = now[i];
	}

	if (same)
		return;

	gAlleycat.ctx->jer_log(gAlleycat.ctx,
		"[Alleycat] state (%s): %s %s follow=%d flee=%d chased=%d cop_near=%d reversing=%d\n",
		why != NULL ? why : "-", now[0] ? "on" : "off", AlleycatBehaviorName(gAlleycat.mode),
		now[2], now[3], now[4], now[5], now[6]);
}

/* ------------------------------------------------------------------ */
/* the HUD corner                                                      */
/* ------------------------------------------------------------------ */

/* jer_hud_panel slots, top left (owner table in jer_hud.h); mp's clock is 5 */
#define ALLEYCAT_HUD_SLOT0		6
#define ALLEYCAT_HUD_LINES		4
#define ALLEYCAT_HUD_REFRESH	10	/* frames between text refreshes: readable, not flickery */

/* a distance in world units as a short "850" / "4.2k" */
static void AlleycatFmtDist(char* out, int max, long long d)
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

static long long AlleycatIsqrt(long long v)
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

static void AlleycatHudClear(void)
{
	int i;

	for (i = 0; i < ALLEYCAT_HUD_LINES; i++)
		jer_hud_panel_clear(ALLEYCAT_HUD_SLOT0 + i);
}

static void AlleycatHudLine(int i, const char* text, int r, int g, int b)
{
	if (text != NULL && text[0] != '\0')
		jer_hud_panel(ALLEYCAT_HUD_SLOT0 + i, JER_HUD_ANCHOR_TOP_LEFT, text, r, g, b);
	else
		jer_hud_panel_clear(ALLEYCAT_HUD_SLOT0 + i);
}

/* Up to four short lines:
 *   1  Alleycat <behavior> [what it is doing]   (colour = behavior, red when chased)
 *   2  speed and the speed it is aiming for
 *   3  the route: waypoints and distance to the goal
 *   4  the situation: nearest cop / followed car / the corner ahead */
void AlleycatHudUpdate(void)
{
	static int shown;
	char line[ALLEYCAT_HUD_LINES][48];
	char d1[16], d2[16];
	CAR_DATA* car;
	int r = 230, g = 230, b = 230;
	int driving;

	if (!gAlleycat.enabled || !gAlleycat.vizHud)
	{
		if (shown)
			AlleycatHudClear();

		shown = 0;
		return;
	}

	if (shown && (gAlleycat.tick % ALLEYCAT_HUD_REFRESH) != 0)
		return;

	shown = 1;
	memset(line, 0, sizeof(line));
	car = AlleycatOwnCar();
	driving = gAlleycat.mode != ALLEYCAT_MODE_MANUAL && car != NULL;

	if (!driving)
	{
		AlleycatHudLine(0, "Alleycat Manual  (L2: settings)", 170, 170, 170);

		for (int i = 1; i < ALLEYCAT_HUD_LINES; i++)
			AlleycatHudLine(i, NULL, 0, 0, 0);

		return;
	}

	/* line 1: what it is doing */
	{
		const char* doing = "";

		if (gAlleycat.recoverFrames > 0)
			doing = " - reversing";
		else if (gAlleycat.panicFlee)
			doing = " - CHASED";
		else if (gAlleycat.evadeActive)
			doing = " - leaving cop";
		else if (gAlleycat.fleeActive)
			doing = " - fleeing";
		else if (gAlleycat.followActive)
			doing = gAlleycat.pursuitRam ? " - ramming" : " - following";
		else if (gAlleycat.lightHold)
			doing = " - red light";

		snprintf(line[0], sizeof(line[0]), "Alleycat %s%s", AlleycatBehaviorName(gAlleycat.mode), doing);

		if (gAlleycat.panicFlee)
			r = 255, g = 90, b = 90;
		else if (gAlleycat.mode == ALLEYCAT_MODE_EVADE)
			r = 255, g = 200, b = 80;
		else
			r = 120, g = 220, b = 255;
	}

	/* line 2: speed */
	if (gAlleycat.speedCap > 0)
		snprintf(line[1], sizeof(line[1]), "Speed %d / %d  cap %d", car->hd.speed, gAlleycat.targetSpeed, gAlleycat.speedCap);
	else
		snprintf(line[1], sizeof(line[1]), "Speed %d / %d", car->hd.speed, gAlleycat.targetSpeed);

	/* line 3: the route */
	{
		long long dx = (long long)gAlleycat.goalX - car->hd.where.t[0];
		long long dz = (long long)gAlleycat.goalZ - car->hd.where.t[2];

		AlleycatFmtDist(d1, sizeof(d1), AlleycatIsqrt(dx * dx + dz * dz));

		if (gAlleycat.havePlan)
			snprintf(line[2], sizeof(line[2]), "Route %d pts%s, goal %s", gAlleycat.path.waypoints,
				gAlleycat.path.complete ? "" : " (part)", d1);
		else
			snprintf(line[2], sizeof(line[2]), "Route: planning");
	}

	/* line 4: the situation */
	if (gAlleycat.mode == ALLEYCAT_MODE_EVADE)
	{
		if (gAlleycat.evadeCopDist > 0)
		{
			AlleycatFmtDist(d1, sizeof(d1), gAlleycat.evadeCopDist);
			snprintf(line[3], sizeof(line[3]), "Cop %s  limit %d", d1, gAlleycat.roadLimit);
		}
		else
			snprintf(line[3], sizeof(line[3]), "No cop near  limit %d", gAlleycat.roadLimit);
	}
	else if (gAlleycat.followActive || gAlleycat.fleeActive)
	{
		AlleycatFmtDist(d1, sizeof(d1), gAlleycat.followDist);
		snprintf(line[3], sizeof(line[3]), "%s %s", gAlleycat.fleeActive ? "Chaser" : "Target", d1);
	}
	else if (gAlleycat.curve.sharp != 0)
	{
		/* heading change in game units (4096 = a full turn), shown in degrees */
		int deg = (gAlleycat.curve.sharp < 0 ? -gAlleycat.curve.sharp : gAlleycat.curve.sharp) * 360 / 4096;

		AlleycatFmtDist(d2, sizeof(d2), gAlleycat.curve.dist);
		snprintf(line[3], sizeof(line[3]), "Corner %d deg in %s  grip %d%%", deg, d2, gAlleycat.gripPct);
	}
	else
		snprintf(line[3], sizeof(line[3]), "Grip %d%%", gAlleycat.gripPct);

	AlleycatHudLine(0, line[0], r, g, b);
	AlleycatHudLine(1, line[1], 230, 230, 230);
	AlleycatHudLine(2, line[2], 230, 230, 230);
	AlleycatHudLine(3, line[3], 230, 230, 230);
}

static int AlleycatOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gAlleycat.tick++;

	/* the menu reads the PHYSICAL pad, so it works while Alleycat drives */
	AlleycatMenuTick();

	/* immunity: keep the wanted level at zero so no cop sirens chase the drive
	 * (the damage half is JER_EVENT_GET_BUDDHA, below) */
	if (AlleycatImmuneActive())
		AlleycatClearWanted();

	if (++gAlleycat.frame % ALLEYCAT_CONFIG_REFRESH == 0)
		AlleycatRefreshConfig();

	/* the road graph is valid only while a level is loaded; drop it between
	 * levels so it is rebuilt for the next one */
	if (!AlleycatMapWorldLoaded())
		gAlleycat.roadBuilt = 0;

	/* the road-graph layer needs the graph: build it once per level when that
	 * layer is on (it used to be built only by a dump, so the layer - on by
	 * default - never drew). A failed build retries twice a second, not every
	 * frame. */
	if (gAlleycat.enabled && gAlleycat.vizRoad && !gAlleycat.roadBuilt && (gAlleycat.tick % 15) == 0)
		AlleycatEnsureRoadGraph();

	/* a dump the menu or the config asked for. Handled before the enabled gate:
	 * asking for a graph is not a driving behavior. */
	if (gAlleycat.dumpRequested)
	{
		if (AlleycatEnsureRoadGraph() && AlleycatDumpRoadGraph(&gAlleycat.road))
			gAlleycat.ctx->jer_log(gAlleycat.ctx,
				"[Alleycat] wrote Alleycat_roadgraph.json/.obj (%d nodes, %d edges)\n",
				gAlleycat.road.nodeCount, gAlleycat.road.edgeCount);
		else
			gAlleycat.ctx->jer_log(gAlleycat.ctx, "[Alleycat] dump requested but no road data yet\n");

		gAlleycat.dumpRequested = 0;
	}

	/* the state line (on change only) and the HUD corner */
	AlleycatLogState("drive");
	AlleycatHudUpdate();

	/* STEER TRACE ONLY: once a second while driving, every number the drive
	 * uses, so a run is legible from the log (and a stalled car shows as an
	 * unchanging pos). Off by default - it is ~35 fields a second. */
	if (gAlleycat.steerTrace > 0 && gAlleycat.enabled && gAlleycat.mode != ALLEYCAT_MODE_MANUAL && (gAlleycat.tick % 30) == 0)
	{
		CAR_DATA* car = AlleycatOwnCar();

		if (car != NULL)
		{
			const ALLEYCAT_BEHAVIOR* b = AlleycatBehaviorFor(gAlleycat.mode);

			gAlleycat.ctx->jer_log(gAlleycat.ctx,
				"[Alleycat] drive mode=%s pos=(%d,%d) speed=%d dir=%d slip=%d yaw=%d tgt=%d grip=%d sharp=%d at=%d wp=%d complete=%d diff=%d bias=%d steer=%d hb=%d pad=%x goal=(%d,%d) aim=(%d,%d) la=%d aimd=%d stuck=%d rec=%d vizlines=%d follow=%d fdist=%d flee=%d police=%d evade=%d cop=%d limit=%d cap=%d light=%d\n",
				b != NULL ? b->name : "?",
				car->hd.where.t[0], car->hd.where.t[2], car->hd.speed, car->hd.direction,
				gAlleycat.carState.slip, gAlleycat.carState.yawRate,
				gAlleycat.targetSpeed, gAlleycat.gripPct,
				gAlleycat.curve.sharp, gAlleycat.curve.dist,
				gAlleycat.path.waypoints, gAlleycat.path.complete, gAlleycat.lastDiff, gAlleycat.lastBias,
				gAlleycat.lastSteerDir, gAlleycat.wantHandbrake, gAlleycat.lastPad,
				gAlleycat.goalX, gAlleycat.goalZ, gAlleycat.aimX, gAlleycat.aimZ,
				gAlleycat.aimLook, gAlleycat.aimDist, gAlleycat.stuckFrames, gAlleycat.recoverFrames,
				AlleycatVizLines(),
				gAlleycat.followActive, gAlleycat.followDist, gAlleycat.fleeActive, gAlleycat.policeActive,
				gAlleycat.evadeActive, gAlleycat.evadeCopDist, gAlleycat.roadLimit, gAlleycat.speedCap, gAlleycat.lightHold);
		}
	}

	return JER_RESULT_CONTINUE;
}

/* a per-run seed so the wander is not identical every launch (Random2 is
 * frame-deterministic). JER_EVENT_GAME_START carries the debug -seed. */
static unsigned int AlleycatSeedFromClock(void)
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

static int AlleycatOnGameStart(void* userdata, void* args)
{
	JER_ARGS_GAME_START* gs = (JER_ARGS_GAME_START*)args;
	unsigned int seed;

	(void)userdata;

	seed = (gs != NULL && gs->seed != 0) ? (unsigned int)gs->seed : AlleycatSeedFromClock();

	AlleycatRandSeed(seed);

	/* a new level: the plan, the goal and the stuck/feeler state all describe the
	 * last one (its coordinates, its heading), so start clean. The road graph
	 * belongs to the level too. */
	AlleycatDriveResetRun();
	AlleycatBehaviorResetRun();
	gAlleycat.roadBuilt = 0;
	AlleycatHudClear();

	gAlleycat.ctx->jer_log(gAlleycat.ctx, "[Alleycat] run seed = %u\n", seed);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* immunity                                                            */
/* ------------------------------------------------------------------ */

/* While Alleycat is driving, the car is invulnerable and the wanted level stays at
 * zero: no damage totals it (see JER_EVENT_GET_BUDDHA) and no cop sirens chase
 * the test drive. Gated on an active, enabled drive so Manual play is normal. */
static int AlleycatImmuneActive(void)
{
	return gAlleycat.enabled && gAlleycat.immune && gAlleycat.mode != ALLEYCAT_MODE_MANUAL;
}

static int AlleycatOnGetBuddha(void* userdata, void* args)
{
	JER_ARGS_QUERY_FLAG* q = (JER_ARGS_QUERY_FLAG*)args;

	(void)userdata;

	if (q != NULL && AlleycatImmuneActive())
		q->result = 1;

	return JER_RESULT_CONTINUE;
}

static void AlleycatClearWanted(void)
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

JER_MODULE_ENTRY(jer_module_Alleycat_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&gAlleycat, 0, sizeof(gAlleycat));
	gAlleycat.ctx = ctx;
	gAlleycat.roadLimit = 56;		/* civ_ai.c speedLimits[0], until Evade reads the road */
	gAlleycat.mapBuiltAt = -1;
	gAlleycat.prevDir = -1;	/* no last heading yet: the first frame has no yaw */
	AlleycatRandSeed(0x12345678u);

	ctx->jer_register_module(ctx,
		"Alleycat",			/* id */
		"Alleycat",			/* name */
		"0.4.0",			/* version (= mod.toml) */
		"Nattdy",			/* author */
		"General-purpose AI driver for the player's car: A* pathfinding, road-graph extraction, joyride, pursuit (follow or ram another player) and cop-evade behaviors, debug visualization and a pause-menu settings page.",	/* description */
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	AlleycatRefreshConfig();
	AlleycatWriteConfigDefaults();
	AlleycatMenuRegister();

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, AlleycatOnCarPad, &gAlleycat, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, AlleycatOnFrame, &gAlleycat, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, AlleycatOnGameStart, &gAlleycat, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_BUDDHA, AlleycatOnGetBuddha, &gAlleycat, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, AlleycatOnDrawWorld, &gAlleycat, 0);

	ctx->jer_log(ctx, "[Alleycat] registered - enabled=%d mode=%d (%s) immune=%d pace=%d pct\n",
		gAlleycat.enabled, gAlleycat.mode, AlleycatBehaviorName(gAlleycat.mode), gAlleycat.immune, gAlleycat.pacePct);
}
