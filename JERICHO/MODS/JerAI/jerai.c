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

#include "players.h"
#include "cars.h"
#include "dr2roads.h"	/* NumDriver2Straights/Curves/Junctions, for the graph log */

#if defined(_MSC_VER)
#include <intrin.h>	/* __rdtsc, for a per-run RNG seed */
#endif

#include <string.h>

/* the single shared state */
JERAI_STATE gJerAi;

/* how often (frames) the tunables are re-read from the config cache */
#define JERAI_CONFIG_REFRESH 30

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

void JerAiRefreshConfig(void)
{
	gJerAi.enabled = jer_config_get_int("JerAI", JERAI_CFG_ENABLED, 1);
	gJerAi.mode = jer_config_get_int("JerAI", JERAI_CFG_MODE, 0);
	gJerAi.pacePct = jer_config_get_int("JerAI", JERAI_CFG_PACE_PCT, 25);
	gJerAi.topSpeed = jer_config_get_int("JerAI", JERAI_CFG_TOP_SPEED, 255);
	gJerAi.runLights = jer_config_get_int("JerAI", JERAI_CFG_RUN_LIGHTS, 1);
	gJerAi.replanMs = jer_config_get_int("JerAI", JERAI_CFG_REPLAN_MS, 700);
	gJerAi.replanMove = jer_config_get_int("JerAI", JERAI_CFG_REPLAN_MOVE, 700);
	gJerAi.lookAhead = jer_config_get_int("JerAI", JERAI_CFG_LOOK_AHEAD, 2500);
	gJerAi.avoidDist = jer_config_get_int("JerAI", JERAI_CFG_AVOID_DIST, 1800);
	gJerAi.copClearance = jer_config_get_int("JerAI", JERAI_CFG_COP_CLEARANCE, 6000);
	gJerAi.maxSteer = jer_config_get_int("JerAI", JERAI_CFG_MAX_STEER, 512);

	gJerAi.vizGrid = jer_config_get_int("JerAI", JERAI_CFG_VIZ_GRID, 0);
	gJerAi.vizRoad = jer_config_get_int("JerAI", JERAI_CFG_VIZ_ROAD, 0);
	gJerAi.vizPath = jer_config_get_int("JerAI", JERAI_CFG_VIZ_PATH, 1);
	gJerAi.vizHud = jer_config_get_int("JerAI", JERAI_CFG_VIZ_HUD, 1);

	/* a one-shot dump request: latch it and clear the key so it fires once */
	if (jer_config_get_int("JerAI", JERAI_CFG_DUMP_NOW, 0) != 0)
	{
		gJerAi.dumpRequested = 1;
		jer_config_set_int("JerAI", JERAI_CFG_DUMP_NOW, 0);
	}
}

void JerAiWriteConfigDefaults(void)
{
	/* Reading first means a value the player edited is written back unchanged;
	 * deliberately NOT re-saved later, or a live edit would be clobbered. */
	jer_config_set_int("JerAI", JERAI_CFG_ENABLED, gJerAi.enabled);
	jer_config_set_int("JerAI", JERAI_CFG_MODE, gJerAi.mode);
	jer_config_set_int("JerAI", JERAI_CFG_PACE_PCT, gJerAi.pacePct);
	jer_config_set_int("JerAI", JERAI_CFG_TOP_SPEED, gJerAi.topSpeed);
	jer_config_set_int("JerAI", JERAI_CFG_RUN_LIGHTS, gJerAi.runLights);
	jer_config_set_int("JerAI", JERAI_CFG_REPLAN_MS, gJerAi.replanMs);
	jer_config_set_int("JerAI", JERAI_CFG_REPLAN_MOVE, gJerAi.replanMove);
	jer_config_set_int("JerAI", JERAI_CFG_LOOK_AHEAD, gJerAi.lookAhead);
	jer_config_set_int("JerAI", JERAI_CFG_AVOID_DIST, gJerAi.avoidDist);
	jer_config_set_int("JerAI", JERAI_CFG_COP_CLEARANCE, gJerAi.copClearance);
	jer_config_set_int("JerAI", JERAI_CFG_MAX_STEER, gJerAi.maxSteer);
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
	/* handled stays 0: the stock path turns the pad into pedals and steering,
	 * which is exactly the point - we feed it a synthetic pad. */

	return JER_RESULT_CONTINUE;
}

static int JerAiOnFrame(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	gJerAi.tick++;

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
				"[JerAI] drive mode=%s pos=(%d,%d) speed=%d dir=%d wp=%d complete=%d diff=%d pad=%x goal=(%d,%d) aim=(%d,%d)\n",
				b != NULL ? b->name : "?",
				car->hd.where.t[0], car->hd.where.t[2], car->hd.speed, car->hd.direction,
				gJerAi.path.waypoints, gJerAi.path.complete, gJerAi.lastDiff, gJerAi.lastPad,
				gJerAi.goalX, gJerAi.goalZ, gJerAi.aimX, gJerAi.aimZ);
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
/* module entry                                                        */
/* ------------------------------------------------------------------ */

JER_MODULE_ENTRY(jer_module_JerAI_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&gJerAi, 0, sizeof(gJerAi));
	gJerAi.ctx = ctx;
	gJerAi.roadLimit = 56;
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

	ctx->jer_log(ctx, "[JerAI] registered - enabled=%d mode=%d (%s) pace=%d pct\n",
		gJerAi.enabled, gJerAi.mode, JerAiBehaviorName(gJerAi.mode), gJerAi.pacePct);
}
