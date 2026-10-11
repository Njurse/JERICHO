/*
 * jerai_internal.h -- glue shared BETWEEN JerAI's own source files.
 *
 * Included only by the engine-side files (everything except the pure
 * jerai_mapgrid / jerai_star / jerai_local, which deliberately include only
 * jerai_map.h so a host test can link them with no engine present).
 *
 * FILE MAP
 *   jerai.c            module entry: registers the module + the engine hooks,
 *                      owns the config refresh and the behavior dispatch
 *   jerai_map.{h,c}    the occupancy grid, built from engine probes
 *   jerai_mapgrid.c    the PURE grid queries + cost model
 *   jerai_star.{h,c}   A* over the grid
 *   jerai_local.{h,c}  the local search ("get back to a road")
 *   jerai_road.{h,c}   the engine road graph (surfaces + ConnectIdx) as
 *                      nodes/edges, for drawing and dumping
 *   jerai_dump.c       writes the road graph to a file
 *   jerai_drive.{h,c}  pad injection + waypoint following
 *   jerai_behavior.{h,c} the modular behavior table (mode -> tick fn)
 *   jerai_viz.c        debug drawing: grid / path / road graph / HUD
 *   jerai_menu.c       the in-game control menu
 */

#ifndef JERAI_INTERNAL_H
#define JERAI_INTERNAL_H

#include "jericho.h"
#include "driver2.h"	/* CAR_DATA, VECTOR, ... */

#include "jerai.h"
#include "jerai_map.h"
#include "jerai_star.h"
#include "jerai_local.h"
#include "jerai_road.h"
#include "jerai_drive.h"

/* ------------------------------------------------------------------ */
/* the module's single live state                                       */
/* ------------------------------------------------------------------ */

typedef struct JERAI_STATE
{
	JERICHO_CONTEXT* ctx;
	int	frame;			/* frames since load, for the config refresh */
	int	tick;			/* frames since load, the driver's clock */

	/* tunables, re-read from JERICHO/CONFIG/JerAI.ini every so often */
	int	enabled;
	int	mode;			/* active behavior id (JERAI_MODE_*, 0 = manual) */
	int	immune;			/* while driving: no damage, no wanted level */
	int	pacePct;		/* joyride pace above the road limit (%) */
	int	topSpeed;		/* flat-out speed cap (0..255) */
	int	runLights;		/* 1 = never park, never stop at lights/yields */
	int	replanMs;		/* route refresh period */
	int	replanMove;		/* goal drift (world units) that forces a replan */
	int	lookAhead;		/* steer at the first waypoint at least this far out */
	int	avoidDist;		/* start slowing for a car ahead within this range */
	int	copClearance;		/* a cop closer than this counts as a threat */
	int	maxSteer;		/* steering lock for direct-steer behaviors */

	/* visualization layers */
	int	vizGrid;
	int	vizRoad;
	int	vizPath;
	int	vizHud;

	/* the current plan. Single-threaded, one plan at a time. */
	JERAI_MAP	map;
	JERAI_PATH	path;
	JERAI_GOAL	goal;
	int	plannedAt;		/* frame the plan was made on */
	int	goalX, goalZ;		/* the goal the plan was made for */
	int	havePlan;		/* 0 before the first successful plan */
	int	incomplete;		/* 1 while the current plan is a partial route */
	int	aimX, aimZ;		/* the point the driver is steering at this frame */
	int	lastDiff;		/* last signed heading error (game units) */
	int	lastBias;		/* last feeler steering bias (game units) */
	int	lastPad;		/* last pad the driver produced */

	/* corner-speed model (phase: car dynamics) */
	int	cornerG;		/* lateral-g budget: folds grip + units into one knob */
	int	brakeG;			/* braking authority, for the braking-distance term */
	int	cornerLookahead;	/* how far ahead the plan is scanned for corners */
	int	targetSpeed;		/* grip-limited speed the driver may carry now */
	int	gripPct;		/* 100 = hooked up, falling as the car slides */

	/* what the car is actually doing, read every driving frame */
	JERAI_CARSTATE carState;

	/* the corner the plan leads into (jerai_drive.c) */
	JERAI_CURVE	curve;

	/* a dump the menu / config asked for, serviced when the world is loaded */
	int	dumpRequested;

	/* debug: a fixed aim heading (game units), or -1 to use the plan */
	int	aimTest;

	/* set by the stuck recovery: a behavior should pick a fresh destination
	 * rather than keep driving at one it cannot reach */
	int	forceNewGoal;

	/* the engine road graph (built on demand; see jerai_road.c) */
	JERAI_ROAD_GRAPH road;
	int	roadBuilt;

	/* recaptured road limit, for the joyride pace boost */
	int	roadLimit;
} JERAI_STATE;

/* One instance, defined in jerai.c, shared by every engine-side file. The game
 * loop is single-threaded, so a plain global is the honest representation. */
extern JERAI_STATE gJerAi;

/* ------------------------------------------------------------------ */
/* shared helpers                                                       */
/* ------------------------------------------------------------------ */

/* Re-read every tunable from the config cache. Called once at registration and
 * periodically from the frame hook. */
void JerAiRefreshConfig(void);

/* Write the tunables back once at boot so JerAI.ini materialises with every
 * knob visible (the debugorbit/testmode pattern). */
void JerAiWriteConfigDefaults(void);

/* Build the engine road graph once, the first time a world is loaded. Returns 1
 * when the graph is available. Logs the counts, and (first build) cross-checks
 * its edges against the engine's own ConnectIdx. */
int JerAiEnsureRoadGraph(void);

/* JER_EVENT_DRAW_WORLD handler (jerai_viz.c): draws the enabled debug layers. */
int JerAiOnDrawWorld(void* userdata, void* args);

/* lines drawn by the last JerAiOnDrawWorld, for the readout */
int JerAiVizLines(void);

/* the in-game control menu (jerai_menu.c): reads the PHYSICAL pad and is drawn
 * over the game. */
void JerAiMenuTick(void);
void JerAiMenuForceOpen(void);
int JerAiOnDrawOverlay(void* userdata, void* args);

#endif /* JERAI_INTERNAL_H */
