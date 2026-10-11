/*
 * jerai.c — module entry point for JerAI.
 *
 * Registers the module and its engine hooks, owns the shared state (gJerAi) and
 * the config refresh. The A* search library lives in jerai_map / jerai_star /
 * jerai_local, the road graph in jerai_road, the driver and behaviors in
 * jerai_drive / jerai_behavior, the debug drawing in jerai_viz and the control
 * menu in jerai_menu (see jerai_internal.h for the file map).
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
	gJerAi.immune = jer_config_get_int("JerAI", JERAI_CFG_IMMUNE, 1);
	gJerAi.aimTest = jer_config_get_int("JerAI", JERAI_CFG_AIM_TEST, -1);
	gJerAi.pacePct = jer_config_get_int("JerAI", JERAI_CFG_PACE_PCT, 25);
	gJerAi.topSpeed = jer_config_get_int("JerAI", JERAI_CFG_TOP_SPEED, 255);
	gJerAi.runLights = jer_config_get_int("JerAI", JERAI_CFG_RUN_LIGHTS, 1);
	gJerAi.replanMs = jer_config_get_int("JerAI", JERAI_CFG_REPLAN_MS, 700);
	gJerAi.replanMove = jer_config_get_int("JerAI", JERAI_CFG_REPLAN_MOVE, 700);
	gJerAi.lookAhead = jer_config_get_int("JerAI", JERAI_CFG_LOOK_AHEAD, 1400);
	gJerAi.avoidDist = jer_config_get_int("JerAI", JERAI_CFG_AVOID_DIST, 1800);
	gJerAi.copClearance = jer_config_get_int("JerAI", JERAI_CFG_COP_CLEARANCE, 6000);
	gJerAi.maxSteer = jer_config_get_int("JerAI", JERAI_CFG_MAX_STEER, 512);
	gJerAi.cornerG = jer_config_get_int("JerAI", JERAI_CFG_CORNER_G, 2);
	gJerAi.brakeG = jer_config_get_int("JerAI", JERAI_CFG_BRAKE_GAIN, 1);
	gJerAi.cornerLookahead = jer_config_get_int("JerAI", JERAI_CFG_CORNER_LOOKAHEAD, 6000);
	gJerAi.speedScale = jer_config_get_int("JerAI", JERAI_CFG_SPEED_SCALE, 100);
	gJerAi.turnCost = jer_config_get_int("JerAI", JERAI_CFG_TURN_COST, 80);

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

	/* the same for forcing the menu open (a headless capture lever) */
	if (jer_config_get_int("JerAI", JERAI_CFG_MENU_OPEN, 0) != 0)
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

	b = JerAiBehaviorFor(gJerAi.mode);

	if (b == NULL)
		return JER_RESULT_CONTINUE;	/* Manual */

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

	/* one-shot probe: the pad the engine offered vs the one we return, so the
	 * bit meanings are never guessed at again */
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

	/* once a second while driving: the car's position and the plan, so a run is
	 * legible from the log (and a stalled car is visible as an unchanging pos) */
	if (gJerAi.enabled && gJerAi.mode != JERAI_MODE_MANUAL && (gJerAi.tick % 30) == 0)
	{
		CAR_DATA* car = JerAiOwnCar();

		if (car != NULL)
		{
			const JERAI_BEHAVIOR* b = JerAiBehaviorFor(gJerAi.mode);

			gJerAi.ctx->jer_log(gJerAi.ctx,
				"[JerAI] drive mode=%s pos=(%d,%d) speed=%d dir=%d slip=%d yaw=%d tgt=%d grip=%d sharp=%d at=%d wp=%d complete=%d diff=%d bias=%d steer=%d hb=%d pad=%x goal=(%d,%d) aim=(%d,%d) vizlines=%d\n",
				b != NULL ? b->name : "?",
				car->hd.where.t[0], car->hd.where.t[2], car->hd.speed, car->hd.direction,
				gJerAi.carState.slip, gJerAi.carState.yawRate,
				gJerAi.targetSpeed, gJerAi.gripPct,
				gJerAi.curve.sharp, gJerAi.curve.dist,
				gJerAi.path.waypoints, gJerAi.path.complete, gJerAi.lastDiff, gJerAi.lastBias,
				gJerAi.lastSteerDir, gJerAi.wantHandbrake, gJerAi.lastPad,
				gJerAi.goalX, gJerAi.goalZ, gJerAi.aimX, gJerAi.aimZ, JerAiVizLines());

			if (gJerAi.vizHud)
			{
				char hud[160];

				snprintf(hud, sizeof(hud),
					"JerAI %s  spd=%d tgt=%d grip=%d  corner=%d@%d  wp=%d %s  nodes=%d edges=%d lines=%d",
					b != NULL ? b->name : "?",
					car->hd.speed, gJerAi.targetSpeed, gJerAi.gripPct,
					gJerAi.curve.sharp, gJerAi.curve.dist,
					gJerAi.path.waypoints,
					gJerAi.path.complete ? "" : "(partial)",
					gJerAi.road.nodeCount, gJerAi.road.edgeCount, JerAiVizLines());

				jer_hud_message(hud, 30);
			}
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
	return (unsigned int)(unsigned long)&marker;
#endif
}

static int JerAiOnGameStart(void* userdata, void* args)
{
	JER_ARGS_GAME_START* gs = (JER_ARGS_GAME_START*)args;
	unsigned int seed;

	(void)userdata;

	seed = (gs != NULL && gs->seed != 0) ? (unsigned int)gs->seed : JerAiSeedFromClock();

	JerAiRandSeed(seed);

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
	gJerAi.roadLimit = 56;
	gJerAi.prevDir = -1;	/* no last heading yet: the first frame has no yaw */
	JerAiRandSeed(0x12345678u);

	ctx->jer_register_module(ctx,
		"JerAI",			/* id */
		"JerAI",			/* name */
		"0.1.0",			/* version */
		"Jaret Ludvik",			/* author */
		"General-purpose AI driver for the player's car: A* pathfinding, road-graph extraction, joyride/random-wander behaviors, debug visualization and a control menu.",	/* description */
		"",				/* dependencies */
		JERICHO_SDK_VERSION);		/* SDK this module was built against */

	JerAiRefreshConfig();
	JerAiWriteConfigDefaults();

	ctx->jer_register_hook(ctx, JER_EVENT_CAR_PAD, JerAiOnCarPad, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, JerAiOnFrame, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, JerAiOnGameStart, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GET_BUDDHA, JerAiOnGetBuddha, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_WORLD, JerAiOnDrawWorld, &gJerAi, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, JerAiOnDrawOverlay, &gJerAi, 0);

	ctx->jer_log(ctx, "[JerAI] registered - enabled=%d mode=%d (%s) immune=%d pace=%d pct\n",
		gJerAi.enabled, gJerAi.mode, JerAiBehaviorName(gJerAi.mode), gJerAi.immune, gJerAi.pacePct);
}
