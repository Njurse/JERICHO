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
 *   jerai_follow.{h,c} who the AI car chases in multiplayer (the mp host)
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
#include "jerai_follow.h"

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

	/* MULTIPLAYER FOLLOW (jerai_follow.c): when a session is live and this
	 * machine is not the host, joyride drives to the HOST's car instead of a
	 * random road sample, then idles within followClearance of it. */
	int	followEnabled;		/* 1 = chase the host (config `follow`) */
	int	followClearance;	/* hold this far from the target (config `follow_clearance`) */
	int	followCar;		/* TEST LEVER: CAR_DATA index to follow, -1 = off */
	int	followRange;		/* give the host up past this distance; 0 = unlimited */
	int	followActive;		/* last frame: we had a target to follow */
	int	followDist;		/* last frame: distance to it (world units, clamped) */

	/* MULTIPLAYER, the host's half (jerai_follow.c): the host RUNS FROM the
	 * nearest other player, so a joining follower has something to chase. */
	int	fleeEnabled;		/* 1 = run from the peers when we are the host */
	int	fleeCar;		/* TEST LEVER: CAR_DATA index to run from, -1 = off */
	int	fleeActive;		/* last frame: we were fleeing */
	int	fleePolice;		/* 1 = run from the police when they are after us */
	int	policeActive;		/* last frame: the cop we were running from was police */
	int	panicFlee;		/* last frame: running flat out (police) - see JerAiSpeedTarget */
	int	panicWheelspin;		/* 1 = may use the wheelspin launch while panicking */
	int	panicOffroad;		/* offroad_cost to use while being chased - see jerai.h */
	int	panicLookAhead;		/* extra aim look-ahead while being chased (world units) */
	int	followCarId;		/* CAR_DATA index being followed/run from, -1 = none */

	int	yawLead;		/* steering damper: frames of yaw the aim leads by */
	int	steerTrace;		/* log a compact steering-loop line every N frames (0 = off) */
	int	lastPred;		/* the damped heading error the steer decision used */
	int	lastWheel;		/* the front wheel angle AFTER the pad was applied (handling.c) */
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
	int	stuckFrames;		/* frames at a standstill, for the recovery trigger */
	int	recoverFrames;	/* frames left in the reverse burst (0 = not recovering) */
	int	aimLook;		/* the look-ahead distance the aim actually used this frame */
	int	aimDist;		/* how far ahead the chosen aim point sits (world units) */
	int	aimPathDir;		/* the PATH's own direction where the aim sits (0..4095) */
	int	aimOff;		/* how far the car is OFF the planned line (cross-track) */
	int	lastSteerDir;		/* -1/0/+1: hysteresis on the steer command */
	int	wantHandbrake;		/* set for a wall-evasion tight turn (applied via handled) */

	/* corner-speed model (phase: car dynamics) */
	int	cornerG;		/* lateral-g budget: folds grip + units into one knob */
	int	brakeG;			/* braking authority, for the braking-distance term */
	int	cornerLookahead;	/* how far ahead the plan is scanned for corners */
	int	speedScale;		/* the target speed as a percentage (100 = as computed) */
	int	turnCost;		/* A* extra cost for a direction change between samples */
	int	offroadCost;		/* A* extra cost for an off-road sample (the road preference) */
	int	targetSpeed;		/* grip-limited speed the driver may carry now */
	int	gripPct;		/* 100 = hooked up, falling as the car slides */

	/* what the car is actually doing, read every driving frame */
	JERAI_CARSTATE carState;
	int	prevDir;		/* last frame's heading, for the yaw in game units */
	int	reposition;		/* stuck: pick a WILD new destination, not a near one */

	/* the corner the plan leads into (jerai_drive.c) */
	JERAI_CURVE	curve;

	/* a dump the menu / config asked for, serviced when the world is loaded */
	int	dumpRequested;

	/* debug: a fixed aim heading (game units), or -1 to use the plan */
	int	aimTest;

	/* set by the stuck recovery: a behavior should pick a fresh destination
	 * rather than keep driving at one it cannot reach */
	int	forceNewGoal;

	/* the follow behavior's tangential detour: while tanActive, drive to
	 * (tanX, tanZ) -- a spot SIDEWAYS to the target -- before re-routing */
	int	tanX, tanZ;
	int	tanActive;

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

/* The same scenery test CellEmpty makes, with the SMASHABLE branch KEPT - it only
 * ADDS the objects CellEmpty skips (light posts, fencing, chairs), so it can
 * never report something safe as blocked. Defined in jerai_map.c; used by the
 * occupancy grid (so the A* routes around them) and by the steering feelers. */
int JerAiMapCellHasSmashable(VECTOR* p, int radius);

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
