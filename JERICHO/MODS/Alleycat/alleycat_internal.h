/*
 * alleycat_internal.h -- glue shared BETWEEN Alleycat's own source files.
 *
 * Included only by the engine-side files (everything except the pure
 * alleycat_mapgrid / alleycat_star / alleycat_local, which deliberately include only
 * alleycat_map.h so a host test can link them with no engine present).
 *
 * FILE MAP
 *   alleycat.c            module entry: registers the module + the engine hooks,
 *                      owns the config refresh and the behavior dispatch
 *   alleycat_map.{h,c}    the occupancy grid, built from engine probes
 *   alleycat_mapgrid.c    the PURE grid queries + cost model
 *   alleycat_star.{h,c}   A* over the grid
 *   alleycat_local.{h,c}  the local search ("get back to a road")
 *   alleycat_road.{h,c}   the engine road graph (surfaces + ConnectIdx) as
 *                      nodes/edges, for drawing and dumping
 *   alleycat_dump.c       writes the road graph to a file
 *   alleycat_drive.{h,c}  pad injection + waypoint following
 *   alleycat_behavior.{h,c} the modular behavior table (mode -> tick fn)
 *   alleycat_follow.{h,c} who the AI car chases in multiplayer (the mp host)
 *   alleycat_viz.c        debug drawing: grid / path / road graph / HUD
 *   alleycat_menu.c       the in-game control menu
 */

#ifndef ALLEYCAT_INTERNAL_H
#define ALLEYCAT_INTERNAL_H

#include "jericho.h"
#include "driver2.h"	/* CAR_DATA, VECTOR, ... */

#include "alleycat.h"
#include "alleycat_map.h"
#include "alleycat_star.h"
#include "alleycat_local.h"
#include "alleycat_road.h"
#include "alleycat_drive.h"
#include "alleycat_follow.h"

/* ------------------------------------------------------------------ */
/* the module's single live state                                       */
/* ------------------------------------------------------------------ */

typedef struct ALLEYCAT_STATE
{
	JERICHO_CONTEXT* ctx;
	int	frame;			/* frames since load, for the config refresh */
	int	tick;			/* frames since load, the driver's clock */

	/* tunables, re-read from JERICHO/CONFIG/Alleycat.ini every so often */
	int	enabled;
	int	mode;			/* active behavior id (ALLEYCAT_MODE_*, 0 = manual) */
	int	immune;			/* while driving: no damage, no wanted level */

	/* MULTIPLAYER FOLLOW (alleycat_follow.c): when a session is live and this
	 * machine is not the host, the PURSUIT behavior drives to the HOST's car
	 * instead of a random road sample, then idles within followClearance of it
	 * (or rams it when pursuitRam is on). */
	int	followEnabled;		/* 1 = chase the host (config `follow`) */
	int	followClearance;	/* hold this far from the target (config `follow_clearance`) */
	int	followCar;		/* TEST LEVER: CAR_DATA index to follow, -1 = off */
	int	followRange;		/* give the host up past this distance; 0 = unlimited */
	int	pursuitRam;		/* 1 = no clearance hold, drive into the target */
	int	followActive;		/* last frame: we had a target to follow */
	int	followDist;		/* last frame: distance to it (world units, clamped) */

	/* MULTIPLAYER, the host's half (alleycat_follow.c): the host RUNS FROM the
	 * nearest other player, so a joining follower has something to chase. */
	int	fleeEnabled;		/* 1 = run from the peers when we are the host */
	int	fleeCar;		/* TEST LEVER: CAR_DATA index to run from, -1 = off */
	int	fleeHost;		/* 1 = a CLIENT runs from the HOST (0 = off, so a
					 * joining AI still CHASES the host by default) */
	int	fleeActive;		/* last frame: we were fleeing */
	int	fleePolice;		/* 1 = run from the police when they are after us */
	int	policeActive;		/* last frame: the cop we were running from was police */
	int	panicFlee;		/* last frame: running flat out (police) - see AlleycatSpeedTarget */
	int	panicWheelspin;		/* 1 = may use the wheelspin launch while panicking */
	int	panicOffroad;		/* offroad_cost to use while being chased - see alleycat.h */
	int	panicLookAhead;		/* extra aim look-ahead while being chased (world units) */
	int	followCarId;		/* CAR_DATA index being followed/run from, -1 = none */

	int	yawLead;		/* steering damper: frames of yaw the aim leads by */
	int	steerTrace;		/* log a compact steering-loop line every N frames (0 = off) */
	int	lastPred;		/* the damped heading error the steer decision used */
	int	lastWheel;		/* the front wheel angle AFTER the pad was applied (handling.c) */
	int	pacePct;		/* Evade: cruise pace above the road limit (%) */
	int	topSpeed;		/* flat-out speed cap (0..255) */
	int	runLights;		/* Evade: 1 = never stop at lights; 0 = stop for red/amber unless chased */
	int	replanMs;		/* route refresh period */
	int	replanMove;		/* goal drift (world units) that forces a replan */
	int	lookAhead;		/* steer at the first waypoint at least this far out */
	int	avoidDist;		/* start slowing for a car ahead within this range */
	int	copClearance;		/* Evade: a cop closer than this counts as a threat */
	int	maxSteer;		/* Evade: wheel-angle cap (352+ = none, see alleycat.h) */

	/* visualization layers */
	int	vizGrid;
	int	vizRoad;
	int	vizPath;
	int	vizHud;

	/* the current plan. Single-threaded, one plan at a time. */
	ALLEYCAT_MAP	map;
	ALLEYCAT_PATH	path;
	ALLEYCAT_GOAL	goal;
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
	ALLEYCAT_CARSTATE carState;
	int	prevDir;		/* last frame's heading, for the yaw in game units */
	int	reposition;		/* stuck: pick a WILD new destination, not a near one */

	/* the corner the plan leads into (alleycat_drive.c) */
	ALLEYCAT_CURVE	curve;

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

	/* the engine road graph (built on demand; see alleycat_road.c) */
	ALLEYCAT_ROAD_GRAPH road;
	int	roadBuilt;

	/* the road's own speed limit where the car is (civ_ai.c speedLimits[] for
	 * the surface's ROAD_SPEED_LIMIT id), re-read by Evade every frame; the
	 * cruise pace is this plus pace_pct */
	int	roadLimit;

	/* EVADE (alleycat_behavior.c): keep clear of the police */
	int	speedCap;		/* cruise cap on the target speed, 0 = none (AlleycatSpeedTarget) */
	int	steerCap;		/* wheel-angle cap on the steering pad, 0 = none */
	int	evadeActive;		/* last frame: a cop was inside cop_clearance */
	int	lightHold;		/* last frame: stopped for a light */
	int	evadeCopDist;		/* last frame: distance to the nearest cop (clamped), 0 = none */

	int	mapBuiltAt;		/* tick of the last occupancy build (one per frame) */
} ALLEYCAT_STATE;

/* One instance, defined in alleycat.c, shared by every engine-side file. The game
 * loop is single-threaded, so a plain global is the honest representation. */
extern ALLEYCAT_STATE gAlleycat;

/* ------------------------------------------------------------------ */
/* shared helpers                                                       */
/* ------------------------------------------------------------------ */

/* Re-read every tunable from the config cache. Called once at registration and
 * periodically from the frame hook. */
void AlleycatRefreshConfig(void);

/* Write the tunables back once at boot so Alleycat.ini materialises with every
 * knob visible (the debugorbit/testmode pattern). */
void AlleycatWriteConfigDefaults(void);

/* Build the engine road graph once, the first time a world is loaded. Returns 1
 * when the graph is available. Logs the counts, and (first build) cross-checks
 * its edges against the engine's own ConnectIdx. */
int AlleycatEnsureRoadGraph(void);

/* The same scenery test CellEmpty makes, with the SMASHABLE branch KEPT - it only
 * ADDS the objects CellEmpty skips (light posts, fencing, chairs), so it can
 * never report something safe as blocked. Defined in alleycat_map.c; used by the
 * occupancy grid (so the A* routes around them) and by the steering feelers. */
int AlleycatMapCellHasSmashable(VECTOR* p, int radius);

/* JER_EVENT_DRAW_WORLD handler (alleycat_viz.c): draws the enabled debug layers. */
int AlleycatOnDrawWorld(void* userdata, void* args);

/* lines drawn by the last AlleycatOnDrawWorld, for the readout */
int AlleycatVizLines(void);

/* the settings page (alleycat_menu.c): a jer_pause_menu page, registered from the
 * module entry. AlleycatMenuTick watches L2 (the PHYSICAL pad) and opens the pause
 * on the page; AlleycatMenuForceOpen does that directly. */
void AlleycatMenuRegister(void);
void AlleycatMenuTick(void);
void AlleycatMenuForceOpen(void);

/* CAR_PAD_* / MPAD_* bits the live car must not see this frame: the keys that
 * closed the pause from the page, while they are still held. 0 otherwise. */
unsigned int AlleycatMenuPadMask(void);

/* alleycat.c: one log line when the drive's state changes (nothing otherwise), and
 * the top-left HUD lines (jer_hud_panel slots 6-9, while viz_hud is on) */
void AlleycatLogState(const char* why);
void AlleycatHudUpdate(void);

#endif /* ALLEYCAT_INTERNAL_H */
