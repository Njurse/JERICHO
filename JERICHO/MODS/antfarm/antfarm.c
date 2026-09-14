/*
 * antfarm.c — "Ant Farm" screensaver / idle mode (JERICHO module).
 *
 * A passive city observer for REDRIVER2. When enabled it:
 *   - cuts off player input (gStopPadReads), hides the HUD (gDoOverlays),
 *     mutes all car/player SFX (SetMasterVolume), disables cop aggression
 *     (CopsAllowed = 0),
 *   - pins the hidden player car to the camera focus and redirects the
 *     region spool there, so geometry + traffic stream to every shot,
 *   - hops to a FAR area of the map on every cut (touring the whole city)
 *     and picks a diverse cinematic shot:
 *       CHASE    : behind-follow on a traffic car, framed to vehicle size
 *       STATIC   : a fixed roadside camera tracking a car as it passes
 *       OVERHEAD : scenic elevated 3/4 view down onto traffic (car or road)
 *       TRIPOD   : parked roadside camera watching a junction/road
 *       FLYOVER  : slow eased dolly along a long straight
 *     Static/overhead styles are weighted over the chase cam.
 *   - while focusing on a car, the camera cycles through multiple angles
 *     (chase behind, chase front, side, overhead, static) every few seconds,
 *     inspired by the replay director's camera modes, before cutting to a
 *     new area.
 *   - optional rogue-car events (off by default): a tiny chance per cut that
 *     the car of interest becomes LEAD AI and tears across the map, with
 *     cops giving chase; the camera follows it until it is totaled.
 *
 * Transitions fade through a semi-transparent wash (the stock FadeGameScreen
 * look) and reselect while black, so the new area streams in without popping.
 *
 * Toggle: F9 (keyboard, PC) or Pause -> Modules -> Ant Farm. Pressing START
 * hands control back and opens the normal pause.
 *
 * Pure JERICHO module — no game files are edited.
 */

#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_pause_menu.h"

#include "driver2.h"
#include "dr2math.h"
#include "players.h"	/* player[] / MainPlayer (spoolXZ, playerType) */
#include "cars.h"		/* car_data[] */
#include "camera.h"		/* camera_position, camera_angle, CameraCar, PointAtTarget */
#include "dr2roads.h"	/* Driver2StraightsPtr, MapHeight */
#include "main.h"		/* gStopPadReads, game_over, FrameCnt */
#include "overlay.h"	/* gDoOverlays */
#include "system.h"		/* current (prim buffer) */
#include "cutscene.h"	/* gInGameCutsceneActive */
#include "glaunch.h"	/* quick_replay, NoPlayerControl */
#include "mission.h"	/* NumPlayers, CopsAllowed */
#include "convert.h"	/* Random2 */
#include "objcoll.h"	/* lineClear, CheckScenaryCollisions */
#include "civ_ai.h"	/* reservedSlots, InitCar */
#include "felony.h"	/* GetPlayerFelony */
#include "sound.h"	/* gMasterVolume, SetMasterVolume */
#include "map.h"	/* units_across_halved, units_down_halved, current_region, regions_across */
#include "spool.h"	/* spoolinfo_offsets, regions_unpacked */
#include "pres.h"	/* SetTextColour, PrintString (overlay captions) */

 /* spool.c:171 — 1 when the barrel regions around the current spool position
  * are all unpacked. Not in a header; the module brings its own extern. */
extern int check_regions_present(void);

#include "antfarm.h"

/* civ_ai.c:970 — the canonical road-segment + distance → x,z sampler.
 * Not declared in any header; the module brings its own extern. */
extern int GetNodePos(DRIVER2_STRAIGHT* straight, DRIVER2_JUNCTION* junction,
	DRIVER2_CURVE* curve, int distAlongPath, CAR_DATA* cp,
	int* x, int* z, int laneNo);

#ifndef PSX
#include <SDL.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h> /* for malloc/free */

#define ANT_MOD_ID "antfarm"

/* ------------------------------------------------------------------ */
/* module RNG                                                         */
/* ------------------------------------------------------------------ */
/* Random2() is a pure function of the frame counter, so every call in a
 * single frame returns the SAME value: the old code's "24 random picks"
 * were 24 identical picks, and every cut's framing was correlated with
 * the frame it happened to land on. We keep our own LCG, seeded once per
 * run - pinned by the engine's debug -seed (so two runs are comparable)
 * and otherwise drawn from ASLR + rdtsc so it varies every launch. */
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#define ANT_HAVE_RDTSC 1
#endif

static unsigned int sRunSeedOverride;	/* 0 = not pinned */
static unsigned int sRngState;
static int sRngInit;

static void AntFarmSetRunSeed(unsigned int seed)
{
	sRunSeedOverride = seed;
	sRngState = 0;
	sRngInit = 0;
}

static unsigned int AntFarmRunSeed(void)
{
	int probe;
	unsigned int s;

	if (sRunSeedOverride != 0)
		return sRunSeedOverride;

	s = (unsigned int)(size_t)&sRngState;	/* ASLR */
	s ^= (unsigned int)(size_t)&probe;		/* stack */

#if defined(ANT_HAVE_RDTSC)
	s ^= (unsigned int)__rdtsc();
	s ^= (unsigned int)(__rdtsc() >> 32);
#endif

	s ^= (unsigned int)Random2(0) << 11;
	s = s * 2654435761u + 2246822519u;		/* avalanche */

	if (s == 0)
		s = 0x9E3779B9u;

	return s;
}

/* advance-and-return, 0..32767 */
static int AntRand(void)
{
	if (!sRngInit)
	{
		sRngState = AntFarmRunSeed();
		sRngInit = 1;
	}

	sRngState = sRngState * 1664525u + 1013904223u;	/* Numerical Recipes LCG */
	return (int)((sRngState >> 16) & 0x7fff);
}

/* inclusive range low..high (order-tolerant) */
static int AntRandRange(int lo, int hi)
{
	if (lo > hi) { int t = lo; lo = hi; hi = t; }
	if (hi == lo) return lo;
	return lo + AntRand() % (hi - lo + 1);
}

/* true with the given percent chance */
static int AntRandChance(int percent)
{
	return (percent > 0) && ((AntRand() % 100) < percent);
}

/* ------------------------------------------------------------------ */
/* camera archetypes                                                  */
/* ------------------------------------------------------------------ */
/* how the camera is driven. Several archetypes share a model and differ only
 * in their ranges (a kerb pass is a roadside camera that happens to be low and
 * long-lensed), so a new angle stays a single table row. */
#define ANT_MODEL_ROADSIDE   0	/* camera parked beside the road, framing it */
#define ANT_MODEL_DOLLY      1	/* travel along the road (flyover/crane) */
#define ANT_MODEL_ORBIT      2	/* circle the subject */
#define ANT_MODEL_FOLLOW     3	/* car-relative chase */
#define ANT_MODEL_TRACK      4	/* fixed roadside camera tracking a car */
#define ANT_MODEL_ATTACH     5	/* rig on the car itself (fender/sill/3-4) */
#define ANT_MODEL_CRANE      6	/* travel along the road while rising */
#define ANT_MODEL_TRIPODZ    7	/* fixed vantage on a car, long lens */
#define ANT_MODEL_COUNT      8

/* Is this model's camera code implemented yet? A style whose model is not
 * implemented is never offered by the picker and never defaults on, so a
 * half-landed archetype cannot be selected. Each new rig flips its case on as
 * its camera lands. */
static int AntFarmStyleImplemented(int model)
{
	switch (model)
	{
	case ANT_MODEL_ROADSIDE:
	case ANT_MODEL_DOLLY:
	case ANT_MODEL_ORBIT:
	case ANT_MODEL_FOLLOW:
	case ANT_MODEL_TRACK:
	case ANT_MODEL_CRANE:
	case ANT_MODEL_ATTACH:
	case ANT_MODEL_TRIPODZ:
		return 1;

	default:
		return 0;
	}
}

/* One row per style. Both the director (selection) and the framing code read
 * from here, so a new camera archetype is a single line. Lo/Hi are inclusive
 * ranges the per-cut randomiser draws from. */
typedef struct ANT_STYLE_DEF
{
	const char* label;	/* menu + log label */
	const char* key;	/* config key suffix: style_<key> */
	int model;		/* ANT_MODEL_* - how the camera is driven */
	int weight;		/* selection weight */
	int roadOnly;		/* 1 = never takes a car subject */
	int carFirst;		/* 1 = takes (and prefers) a car subject */
	int behind;		/* attached rigs: 1 = sit behind the car */
	int zoom;		/* 1 = slow lens push-in then out across the shot */
	int heightLo, heightHi;	/* camera elevation above ground/road */
	int scrZLo, scrZHi;	/* FOV (projection distance); smaller = wider.
				 * For a zoom style these are the ramp's two ends. */
	int sideLo, sideHi;	/* lateral offset: rig offset, or the roadside margin */
	int fwdLo, fwdHi;	/* longitudinal offset (attached rigs) */
	int aimLo, aimHi;	/* how far ahead of the subject to look */
	int orbitLo, orbitHi;	/* azimuth sweep amplitude (0 = fixed) */
	int across;		/* 1 = aim across the road, not along it */
	int settle;		/* camera settle divisor (bigger = calmer) */
	int dwell;		/* scene-interest dwell multiplier, x100 */
	int nearWater;		/* 1 = prefer a road that runs beside water */
} ANT_STYLE_DEF;

/* The archetype table. Weight, subject capability and all the per-cut
 * framing ranges live here; picking and framing both read it. */
static const ANT_STYLE_DEF antStyleDefs[ANTFARM_STYLE_COUNT] = {
	/* label          key          model            wt rOnly car beh zoom  hLo  hHi scrLo scrHi sLo sHi  fLo  fHi aimLo aimHi orbLo orbHi acr set dwell  nw */
	{ "Chase cam",    "chase",     ANT_MODEL_FOLLOW,   4,   0,   1,  0,  0,  180,  340,  260,  276,    0,    0,   0,    0,    0,    0,    0,    0,  0,   8,   90 },
	{ "Static track", "static",    ANT_MODEL_TRACK,   22,   0,   1,  0,  0,  180,  300,  258,  280,    0,    0,   0,    0,    0,    0,    0,    0,  0,   6,  120 },
	{ "Overhead",     "overhead",  ANT_MODEL_ROADSIDE,26,   0,   0,  0,  0,  400,  700,  218,  242,  180,  360,   0,    0,  900, 1100,    0,    0,  0,  14,  130 },
	{ "Tripod",       "tripod",    ANT_MODEL_ROADSIDE,16,   1,   0,  0,  0,  140,  260,  238,  268,  135,  270,   0,    0,  900, 1100,  820, 1140,  0,  16,  120 },
	{ "Flyover",      "flyover",   ANT_MODEL_DOLLY,   12,   1,   0,  0,  0,  400,  650,  248,  272,    0,    0,   0,    0,    0,    0,    0,    0,  0,  18,  140 },
	{ "Orbit",        "orbit",     ANT_MODEL_ORBIT,   12,   0,   0,  0,  0,  200,  440,  232,  262,    0,    0,   0,    0,    0,    0,    0,    0,  0,  20,  140 },
	{ "Crane",        "crane",     ANT_MODEL_CRANE,  10,   1,   0,  0,  0,   90,  820,  240,  270,    0,    0,   0,    0,    0,    0,    0,    0,  0,  18,  150 },
	{ "Ant level",    "low",       ANT_MODEL_ROADSIDE,10,   1,   0,  0,  0,   45,   95,  250,  282,  180,  360,   0,    0,    0,  220,    0,    0,  1,  16,  120 },
	/* --- car-attached rigs (damped, yaw-only: see ANT_MODEL_ATTACH) --- */
	{ "Fender",       "fender",    ANT_MODEL_ATTACH,  12,   0,   1,  0,  0,   45,   85,  250,  272,   20,   60, 260,  380,  900, 1200,    0,    0,  0,   3,  110 },
	{ "Sill",         "sill",      ANT_MODEL_ATTACH,  12,   0,   1,  0,  0,   40,   75,  245,  265,  240,  330, -60,   60,  400,  700,    0,    0,  0,   3,  110 },
	{ "Nose 3/4",     "nose34",    ANT_MODEL_ATTACH,  12,   0,   1,  0,  0,   90,  150,  250,  272,  200,  300, 320,  480,  500,  800,    0,    0,  0,   5,  120 },
	{ "Tail 3/4",     "tail34",    ANT_MODEL_ATTACH,  12,   0,   1,  1,  0,   90,  150,  245,  268,  180,  280, 260,  420,  500,  800,    0,    0,  0,   5,  120 },
	/* --- free / static angles --- */
	{ "Kerb pass",    "kerb",      ANT_MODEL_ROADSIDE,12,   1,   0,  0,  0,   40,   80,  290,  320,  200,  320,   0,    0,    0,  200,    0,    0,  1,  14,  120 },
	{ "Tripod zoom",  "tripzoom",  ANT_MODEL_TRIPODZ, 12,   0,   1,  0,  1,  120,  220,  235,  300,  200,  360, 500, 1500,  900, 1200,  300,  600,  0,  18,  150 },
	{ "Far pan",      "farpan",    ANT_MODEL_TRIPODZ, 10,   0,   1,  0,  1,  200,  340,  290,  340, 1400, 2200, 2000, 4000,  900, 1300,  400,  900,  0,  20,  150 },
	{ "Waterfront",   "water",     ANT_MODEL_DOLLY,   10,   1,   0,  0,  0,   60,  180,  250,  275,    0,    0,   0,    0,  700, 1000,    0,    0,  0,  16,  150,  1 },
};

/* ------------------------------------------------------------------ */
/* cached usable road list (fast random & nearest)                    */
/* ------------------------------------------------------------------ */
static int* g_usableRoads = NULL;
static int g_numUsableRoads = 0;
static int g_usableRoadsAlloc = 0;
static int g_usableRoadsBuiltFor = -1;	/* NumDriver2Straights the cache was built from */

/* spool.c / map.c — unpacked status of each region (1 = loaded) */
extern int regions_unpacked[];
/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

/* Car camera modes (angles) - inspired by replay director */
#define CAR_MODE_CHASE_BEHIND 0
#define CAR_MODE_CHASE_FRONT  1
#define CAR_MODE_SIDE_LEFT    2
#define CAR_MODE_SIDE_RIGHT   3
#define CAR_MODE_OVERHEAD     4
#define CAR_MODE_STATIC       5
#define CAR_MODE_COUNT        6

typedef struct ANTFARM_STATE
{
	JERICHO_CONTEXT* ctx;

	int active;		/* screensaver running */
	int pendingEnable;	/* antfarm.ini said enabled — activate when playable */
	int camSnapped;		/* camera has been snapped to the current target */

	int style;		/* ANTFARM_STYLE_* */
	int targetKind;		/* ANTFARM_TARGET_* */
	int state;		/* ANTFARM_STATE_* */
	int fade;		/* 0..255 overlay wash intensity */
	unsigned long stateStart;	/* when the current state began (ms) */
	unsigned long shotStart;	/* when the current shot's fade-in began (ms) */

	int intervalMs;					/* seconds per visible shot (cut interval) */
	int carModeIntervalMs;			/* seconds per car mode change */
	int carModesPerCut;				/* number of mode changes before a cut */
	int stylesEnabled[ANTFARM_STYLE_COUNT];
	int leadEnabled;	/* rogue-car events allowed */
	int leadChance;		/* percent chance per cut */

	/* the shot's subject */
	int targetCarId;	/* CAR targets: car_data index */
	int roadSurfId;		/* ROAD targets: straight index (== surfId) */
	int roadLane;		/* lane index sampled on that road */
	int roadDist;		/* distAlongPath on the road (world units) */
	int junctionCorner;	/* shot sits just past a junction mouth, looking back */
	VECTOR targetPos;	/* world-space focus (also the spool content) */
	VECTOR areaPos;		/* the far-area anchor of the current cut */

	/* per-shot framing (these are modified by car modes) */
	int shotSideSign;	/* +1/-1 — which side of the road the camera sits */
	int shotMargin;		/* clearance from the road half-width edge */
	int shotHeight;		/* camera elevation above the road/ground */
	int shotFwd;		/* attached rigs: longitudinal offset from the car */
	int shotSide;		/* attached rigs: lateral offset from the car */
	int shotLookAhead;	/* how far ahead of the camera the aim sits */
	int shotOrbitAmp;	/* tripod azimuth sweep amplitude (0 = static) */
	int shotOrbitPhase;	/* orbit phase seed */
	int shotScrZ;		/* per-shot FOV (projection distance) */
	int shotZoomTo;		/* a zoom row's other lens end (== shotScrZ when it holds) */
	int armFrac;		/* smoothed LOS pull-back fraction (256 = full arm) */

	/* Car mode cycling */
	int carMode;				/* current CAR_MODE_* */
	int carModeIndex;			/* how many modes have been shown this cut */
	unsigned long carModeStart;	/* when the current car mode began */
	int carModeDurationMs;		/* configurable duration per mode */

	VECTOR trackCamPos;	/* STATIC style: the fixed roadside camera spot */
	int trackPlaced;	/* the static spot has been placed */

	VECTOR spool;		/* what MainPlayer.spoolXZ points at while active */

	VECTOR camPos;		/* smoothed camera position */
	VECTOR aimPos;		/* current aim point (render space) */
	SVECTOR camAngle;	/* smoothed camera angle */

	/* cut staging */
	int cutInit;		/* CUT state has planned this cut */
	int cutWaitForCar;	/* CUT is waiting for traffic near the new area */
	unsigned long cutStart;	/* when the CUT state began (ms) */
	unsigned long cutEnter;	/* when this CUT first began (never reset) */
	long long jumpDist2;	/* squared jump distance (for hold scaling) */
	int streamRetries;	/* far-area re-picks on streaming timeouts */
	int streamDone;		/* the new area's regions have been accepted+loaded */
	int shotPlanned;	/* 1 when the shot for this cut has been fully planned */

	/* rogue-car (lead AI) event */
	int leadMode;		/* following a rogue car until it is totaled */
	int leadEnding;		/* totaled — holding on the wreck before moving on */
	unsigned long leadEndStart;

	/* engine state saved/restored around activation */
	int savedStopPadReads;
	int savedDoOverlays;
	VECTOR* savedSpoolXZ;
	int savedCopsAllowed;
	int savedMasterVolume;
	int savedScrZ;		/* projection distance, restored on exit (was leaked) */
	int savedCameraCar;	/* CameraCar, restored on exit (was leaked) */
	int fovCurrent;		/* smoothed scr_z so the lens breathes instead of jumping */
	int rollOn;		/* subtle horizon roll for a less rigid frame */
	unsigned long stillSince;	/* when the subject of a rig shot stopped moving */

	/* fixed vantage for the tripod-zoom / far-pan shots, resolved once */
	VECTOR shotVantage;
	int vantageSet;

	/* void guard: while the shot's region is not resident, hold the previous
	 * camera instead of drawing an unloaded one */
	unsigned long holdSince;
	int voidHolds;

	/* presentation dressing */
	int letterbox;		/* draw the soft cinematic bars */
	int captions;		/* show the occasional place-name caption */
	unsigned long captionUntil;	/* when the current caption fades out */

	/* director state: a rolling memory of recent styles so consecutive cuts
	 * never repeat, plus the interest-scaled dwell of the current shot */
	int recentStyles[ANTFARM_STYLE_MEMORY];
	int recentCount;
	int recentCars[ANTFARM_STYLE_MEMORY];	/* cars already framed recently */
	int recentCarCount;
	int dwellMs;		/* this shot's visible time, interest-scaled */
	int dissolve;		/* 1 = cross-dissolve through grey rather than black */

	/* player car saved/restored around activation (teleport + hide) */
	int savedPlayerCarControlType;
	int savedPlayerReservedSlot;
	long savedPlayerCarPos[3];
	int savedPlayerFelony;	/* short on the car; int here */

	int cutCount;		/* diagnostic counter */
	int f9Down;		/* F9 edge detection */
} ANTFARM_STATE;

static ANTFARM_STATE s;

/* ------------------------------------------------------------------ */
/* forward declarations for functions used before they are defined    */
/* ------------------------------------------------------------------ */
static void AntFarmPlanShot(void);
static void AntFarmPinPlayerCar(void);
static void AntFarmSetupRoadShot(void);
static void AntFarmInitShotVars(void);
static int AntFarmPickFarArea(void);
static int AntFarmPickRoadNear(int x, int z);
static int AntFarmTrafficNear(const VECTOR* pos, int radius);
static void AntFarmPickStyleAndTarget(void);
static void AntFarmBuildRoadCache(void);
static void AntFarmStaticTrack(CAR_DATA* cp, VECTOR* desired);

/* ------------------------------------------------------------------ */
/* timing                                                             */
/* ------------------------------------------------------------------ */

static unsigned long AntTicks(void)
{
#ifndef PSX
	return (unsigned long)SDL_GetTicks();
#else
	return (unsigned long)FrameCnt * 1000 / 30;
#endif
}

/* the cut interval is config-driven */
static int AntIntervalMs(void)
{
	return (s.intervalMs > 0) ? s.intervalMs : ANTFARM_DEFAULT_INTERVAL * 1000;
}

/* car mode interval */
static int AntCarModeIntervalMs(void)
{
	return (s.carModeIntervalMs > 0) ? s.carModeIntervalMs : 6000; /* default 6 sec */
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static CAR_DATA* AntFarmValidCar(void)
{
	if (s.targetCarId < 0 || s.targetCarId >= MAX_CARS)
		return NULL;

	CAR_DATA* cp = &car_data[s.targetCarId];

	if (cp->controlType != CONTROL_TYPE_CIV_AI &&
		cp->controlType != CONTROL_TYPE_LEAD_AI)
		return NULL;

	return cp;
}

static int AntFarmMapHeight(int x, int z)
{
	VECTOR p;
	int h;

	p.vx = x;
	p.vy = 0;
	p.vz = z;

	h = MapHeight(&p);
	return (h != 0) ? h : 200; /* Fallback default height to avoid complete void drop */
}

/* ------------------------------------------------------------------ */
/* ROAD CACHE                                                         */
/* ------------------------------------------------------------------ */

/* Build the list of roads that can be used for shots (angle<2048, length>400) */
static void AntFarmBuildRoadCache(void)
{
	int n = NumDriver2Straights;

	g_usableRoadsBuiltFor = n;

	if (n <= 0) {
		g_numUsableRoads = 0;
		return;
	}

	if (g_usableRoadsAlloc < n) {
		int* grown = (int*)malloc((size_t)n * sizeof(int));

		if (grown == NULL) {
			/* keep the old (smaller) cache rather than dereferencing NULL */
			g_numUsableRoads = 0;
			return;
		}

		if (g_usableRoads) free(g_usableRoads);
		g_usableRoads = grown;
		g_usableRoadsAlloc = n;
	}

	{
		int count = 0;
		int i;

		for (i = 0; i < n; i++) {
			DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[i];

			if (rd->length > 400 && rd->angle < 2048)
				g_usableRoads[count++] = i;
		}

		g_numUsableRoads = count;
	}
}

/* rebuild the cache whenever the level's straight count changes, not just
 * when it grows: a level change to one with FEWER straights left the old
 * indices in place, which indexes Driver2StraightsPtr out of bounds */
static void AntFarmEnsureRoadCache(void)
{
	if (g_usableRoadsBuiltFor != NumDriver2Straights || g_numUsableRoads == 0)
		AntFarmBuildRoadCache();
}

/* pick a straight at least minLen long, using the cache */
static int AntFarmPickSurface(int minLen)
{
	if (g_numUsableRoads == 0)
		return -1;

	/* Try up to 24 random picks from the cache */
	for (int i = 0; i < 24; i++) {
		int idx = g_usableRoads[AntRand() % g_numUsableRoads];
		if (Driver2StraightsPtr[idx].length > minLen)
			return idx;
	}

	/* Fallback: scan all usable roads for the longest that meets minLen */
	int best = -1, bestLen = 0;
	for (int i = 0; i < g_numUsableRoads; i++) {
		int idx = g_usableRoads[i];
		int len = Driver2StraightsPtr[idx].length;
		if (len > minLen && len > bestLen) {
			bestLen = len;
			best = idx;
		}
	}
	return best;
}

/* Does this straight run beside water? The engine's sea is a single flat plane
 * (GetSeaPlane, dr2roads.c:15) reached through the surface lookup, so probe a
 * few points along the road for a water or beach surface. */
static int AntFarmRoadNearWater(DRIVER2_STRAIGHT* rd)
{
	int i;

	if (rd->length < 600)
		return 0;

	for (i = 0; i <= 3; i++)
	{
		int x = 0x7fffffff, z = 0x7fffffff;
		VECTOR p;
		int surf;

		GetNodePos(rd, NULL, NULL, (rd->length * i) / 3, NULL, &x, &z, 0);

		if (x == 0x7fffffff || z == 0x7fffffff)
			continue;

		p.vx = x;
		p.vy = 0;
		p.vz = z;

		surf = GetSurfaceIndex(&p);

		if (surf == SURF_WATER || surf == SURF_DEEPWATER || surf == SURF_SAND)
			return 1;
	}

	return 0;
}

/* Nearest usable road that actually runs beside water, or -1 if none is. */
static int AntFarmPickWaterRoad(int x, int z)
{
	int best = -1, i;
	long long bestD2 = -1;

	for (i = 0; i < g_numUsableRoads; i++)
	{
		int idx = g_usableRoads[i];
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
		long long dx, dz, d2;

		if (!AntFarmRoadNearWater(rd))
			continue;

		dx = (long long)rd->Midx - x;
		dz = (long long)rd->Midz - z;
		d2 = dx * dx + dz * dz;

		if (bestD2 < 0 || d2 < bestD2)
		{
			bestD2 = d2;
			best = idx;
		}
	}

	return best;
}

/* Pick the closest usable road to (x,z) */
static int AntFarmPickRoadNear(int x, int z)
{
	if (g_numUsableRoads == 0)
		return -1;

	int best = -1;
	long long bestD2 = -1;
	for (int i = 0; i < g_numUsableRoads; i++) {
		int idx = g_usableRoads[i];
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
		long long dx = (long long)rd->Midx - x;
		long long dz = (long long)rd->Midz - z;
		long long d2 = dx * dx + dz * dz;
		if (bestD2 < 0 || d2 < bestD2) {
			bestD2 = d2;
			best = idx;
		}
	}
	return best;
}

/* sample a point ON a road surface at a lane + distance along it, and the
 * traffic heading for that lane. x/z come out in WORLD space (positive-up);
 * the heading is 0..4095. */
static int AntFarmShotRoadPoint(int surfId, int distAlong, int laneNo,
	int* x, int* z, int* heading)
{
	DRIVER2_ROAD_INFO ri;
	DRIVER2_STRAIGHT* straight;
	DRIVER2_CURVE* curve;
	int dirBit;

	if (surfId < 0 || GetSurfaceRoadInfo(&ri, surfId) == 0)
		return 0;

	straight = ri.straight;
	curve = ri.curve;

	if (straight == NULL && curve == NULL)
		return 0;	/* junctions have no centre of their own */

	/* GetNodePos() declines to write its outputs for some straight segments
	 * (angle >= 2048 with no curve), which silently left the shot anchored at
	 * the world origin — the camera then flew off toward the map centre. Seed
	 * a sentinel and treat "untouched" as a failed sample. */
	*x = 0x7fffffff;
	*z = 0x7fffffff;

	GetNodePos(straight, NULL, curve, distAlong, NULL, x, z, laneNo);

	if (*x == 0x7fffffff || *z == 0x7fffffff)
		return 0;

	/* traffic heading for the sampled lane */
	if (straight)
	{
		dirBit = ROAD_LANE_DIR(straight, laneNo);
		*heading = (dirBit != 0) ? ((straight->angle + 2048) & 0xfff) : (straight->angle & 0xfff);
	}
	else
	{
		int ang = (distAlong + curve->start) & 0xfff;
		dirBit = ROAD_LANE_DIR(curve, laneNo);
		*heading = (dirBit != 0) ? ((ang - 1024) & 0xfff) : ((ang + 1024) & 0xfff);
	}

	return 1;
}

/* resolve the shot's road reference into a RENDER-space point + heading */
static int AntFarmShotRoadRender(int distAlong, VECTOR* roadPt, int* heading)
{
	int x, z;

	if (AntFarmShotRoadPoint(s.roadSurfId, distAlong, s.roadLane, &x, &z, heading) == 0)
		return 0;

	roadPt->vx = x;
	roadPt->vy = -AntFarmMapHeight(x, z);	/* render ground = -world ground */
	roadPt->vz = z;

	return 1;
}

/* keep positions inside the playable map so the camera never reaches the
 * nodraw skybox extremes at the world edge, and never dives below the
 * ground into the void (render Y is negative-up: ground = -MapHeight) */
static void AntFarmClampToWorld(VECTOR* v)
{
	int maxX = units_across_halved - 4000;
	int maxZ = units_down_halved - 4000;

	if (v->vx < -maxX)
		v->vx = -maxX;

	if (v->vx > maxX)
		v->vx = maxX;

	if (v->vz < -maxZ)
		v->vz = -maxZ;

	if (v->vz > maxZ)
		v->vz = maxZ;
}

/* clamp a RENDER-space position to just above the ground (render Y is
 * negative-up: ground = -MapHeight, so 'above' = more negative) */
static void AntFarmClampAboveGround(VECTOR* v)
{
	int ground = -AntFarmMapHeight(v->vx, v->vz);

	if (v->vy > ground - 60)
		v->vy = ground - 60;
}

/* (AntFarmWorldPresent used to live here. It probed MapHeight() in a ring and
 * treated 0 as "not loaded" — but AntFarmMapHeight() maps 0 to a 200 fallback,
 * so the test could never fire. The region-residency gate above is the real
 * signal, so it was removed rather than left as a no-op.) */

/* the region index ControlMap computes for a world position */
static int AntFarmRegionOf(const VECTOR* pos)
{
	int cellx = (pos->vx + units_across_halved) / MAP_CELL_SIZE;
	int cellz = (pos->vz + units_down_halved) / MAP_CELL_SIZE;
	int rx = cellx / MAP_REGION_SIZE;
	int rz = cellz / MAP_REGION_SIZE;

	if (rx < 0) rx = 0;
	if (rx >= regions_across) rx = regions_across - 1;
	if (rz < 0) rz = 0;
	if (rz >= regions_down) rz = regions_down - 1;

	return rx + rz * regions_across;
}

/* Is this region number currently resident? The engine keeps FOUR unpacked
 * regions, indexed by BARREL slot — (region_x & 1) + (region_z & 1) * 2 —
 * each holding the region number loaded into it (-1 for none); see
 * map.c:294-303 and spool.c:1653. The old gate treated regions_unpacked[] as
 * a per-region map and indexed it with an absolute region number, so it read
 * far out of bounds and never actually failed: the camera was faded in over
 * geometry that had not streamed — the skybox/nodraw void. */
static int AntFarmRegionUnpacked(int region)
{
	int rx = region % regions_across;
	int rz = region / regions_across;
	int barrel = (rx & 1) + (rz & 1) * 2;

	return regions_unpacked[barrel] == region;
}

/* Is the shot's own region ready to draw from? The engine only ever keeps
 * FOUR regions resident (regions_unpacked[4] / loading_region[4] /
 * PVS_Buffers[4] — a 2x2 barrel window), so the old "centre + 4 cardinal
 * neighbours" test could NEVER be satisfied: every cut burned its stream
 * retries and then the black cap before forcing a fallback, which is why the
 * screensaver sat on black/grey and the shot never actually moved. The
 * achievable test is: this region has data, and it is the region currently
 * unpacked into its barrel slot. */
static int AntFarmRegionsReady(int centerRegion)
{
	int totalRegions = regions_across * regions_down;

	if (centerRegion < 0 || centerRegion >= totalRegions)
		return 0;

	if (spoolinfo_offsets[centerRegion] == 0xffff)
		return 0;		/* no data here at all — re-pick instead */

	return AntFarmRegionUnpacked(centerRegion);
}

/* does this region have spool data at all? (0xffff = none — approaching it
 * would show a void forever) */
static int AntFarmRegionHasData(int region)
{
	int totalRegions = regions_across * regions_down;
	if (region < 0 || region >= totalRegions)
		return 0;
	return spoolinfo_offsets[region] != 0xffff;
}

/* Can a shot be set up here at all? That is a question about DATA, not about
 * residency: the engine can only be made to stream a region by pointing the
 * spool at it, so a picker that demanded the destination already be resident
 * could never move anywhere (it fell back to the camera's own position, which
 * is why the tour stayed parked in one spot). Residency is waited for
 * separately, in the CUT state. */
static int AntFarmPositionHasData(const VECTOR* pos)
{
	return AntFarmRegionHasData(AntFarmRegionOf(pos));
}

/* pick the NEAREST usable straight to the camera so the very first shot
 * starts instantly on a real road that is always inside the loaded area */
static void AntFarmPickNearArea(void)
{
	int bestIdx = -1;
	long long bestD2 = -1;
	int i;

	for (i = 0; i < NumDriver2Straights; i++)
	{
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[i];
		long long dx, dz, d2;

		if (rd->length <= 400 || rd->angle >= 2048)
			continue;

		dx = (long long)rd->Midx - camera_position.vx;
		dz = (long long)rd->Midz - camera_position.vz;
		d2 = dx * dx + dz * dz;

		if (bestD2 < 0 || d2 < bestD2)
		{
			bestD2 = d2;
			bestIdx = i;
		}
	}

	if (bestIdx < 0)
		return;

	s.areaPos.vx = Driver2StraightsPtr[bestIdx].Midx;
	s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
	s.areaPos.vz = Driver2StraightsPtr[bestIdx].Midz;
	AntFarmClampToWorld(&s.areaPos);
}

/* pick a road – preferably in a different region, and ensure the region is loaded.
 * If no loaded region can be found, fallback to the current camera position (which is definitely loaded). */
/* how many civilian cars are actually moving near a point - a liveliness
 * signal the picker can score on (the engine exposes no landmark table) */
static int AntFarmTrafficNear(const VECTOR* pos, int radius)
{
	int i, n = 0;
	long long r2 = (long long)radius * radius;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		long long dx, dz;

		if (cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;

		if (ABS(cp->hd.speed) <= 8)
			continue;

		dx = (long long)cp->hd.where.t[0] - pos->vx;
		dz = (long long)cp->hd.where.t[2] - pos->vz;

		if (dx * dx + dz * dz <= r2)
			n++;
	}

	return n;
}

/* How interesting is this road as a shot subject? Scored only from signals the
 * engine's data really offers - there is no per-level landmark or POI table,
 * and no junction surface id - so: water beside the road, sheer length, and
 * traffic actually moving there. */
static int AntFarmRoadInterest(int idx, const VECTOR* pos)
{
	DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
	int score = 0;
	int traffic;

	if (rd->length > 2600)
		score += 26;
	else if (rd->length > 1500)
		score += 14;

	if (AntFarmRoadNearWater(rd))
		score += 45;

	traffic = AntFarmTrafficNear(pos, 6000);
	if (traffic > 6)
		traffic = 6;

	score += traffic * 6;

	return score;
}

static int AntFarmPickFarArea(void)
{
	if (g_numUsableRoads == 0) {
		/* No usable roads at all – fallback to camera position (which is loaded) */
		s.areaPos.vx = camera_position.vx;
		s.areaPos.vz = camera_position.vz;
		s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
		AntFarmClampToWorld(&s.areaPos);
		return 1;
	}

	int curRegion = AntFarmRegionOf(&s.targetPos);

	/* Sample a handful of candidates and take the most interesting one, with a
	 * penalty for staying in the region we are already in: the tour has to
	 * move, and a long waterfront straight with traffic on it beats a random
	 * back street that happens to come up first. */
	{
		int best = -1, bestScore = -1, attempt;

		for (attempt = 0; attempt < 14; attempt++) {
			int idx = g_usableRoads[AntRand() % g_numUsableRoads];
			DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
			VECTOR pos;
			int score;

			if (rd->length < 200)
				continue;   /* skip very short ones */

			pos.vx = rd->Midx;
			pos.vy = AntFarmMapHeight(rd->Midx, rd->Midz);
			pos.vz = rd->Midz;
			AntFarmClampToWorld(&pos);

			/* the destination must have data — residency comes later */
			if (!AntFarmPositionHasData(&pos))
				continue;

			score = AntFarmRoadInterest(idx, &pos);

			if (AntFarmRegionOf(&pos) == curRegion)
				score -= 20;

			if (score > bestScore) {
				bestScore = score;
				best = idx;
				s.areaPos = pos;
			}
		}

		if (best >= 0)
			return 1;
	}

	/* Fallback: try any usable road that is loaded, even if same region */
	for (int attempt = 0; attempt < 30; attempt++) {
		int idx = g_usableRoads[AntRand() % g_numUsableRoads];
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
		if (rd->length < 200) continue;
		VECTOR pos;
		pos.vx = rd->Midx;
		pos.vy = AntFarmMapHeight(rd->Midx, rd->Midz);
		pos.vz = rd->Midz;
		AntFarmClampToWorld(&pos);
		if (AntFarmPositionHasData(&pos)) {
			s.areaPos = pos;
			return 1;
		}
	}

	/* Ultimate fallback: use current camera position (loaded) */
	s.areaPos.vx = camera_position.vx;
	s.areaPos.vz = camera_position.vz;
	s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
	AntFarmClampToWorld(&s.areaPos);
	return 1;
}

/* pick a moving civilian car near a point (world units), or -1 */
/* has this car been the subject in the last few cuts? */
static int AntFarmCarRecent(int carId)
{
	int i;

	for (i = 0; i < s.recentCarCount; i++)
		if (s.recentCars[i] == carId)
			return 1;

	return 0;
}

static void AntFarmRememberCar(int carId)
{
	int i;

	if (carId < 0)
		return;

	for (i = ANTFARM_STYLE_MEMORY - 1; i > 0; i--)
		s.recentCars[i] = s.recentCars[i - 1];

	s.recentCars[0] = carId;

	if (s.recentCarCount < ANTFARM_STYLE_MEMORY)
		s.recentCarCount++;
}

/* Pick a moving civilian car near the area. `minSpeed` lets a rig demand a car
 * that is genuinely underway, and a car framed in the last few cuts is heavily
 * de-weighted so the same sedan is not followed all session. */
static int AntFarmPickCarNear(const VECTOR* area, int radius, int minSpeed)
{
	int i, n = 0, pick = -1;
	long long r2 = (long long)radius * radius;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		long long dx, dz;
		int weight;

		if (cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;

		if (ABS(cp->hd.speed) <= minSpeed)
			continue;

		dx = (long long)cp->hd.where.t[0] - area->vx;
		dz = (long long)cp->hd.where.t[2] - area->vz;

		if (dx * dx + dz * dz > r2)
			continue;

		weight = AntFarmCarRecent(i) ? 1 : 6;
		n += weight;

		if (AntRand() % n < weight)
			pick = i;
	}

	return pick;
}

/* vehicle length/height for framing (colBox half-lengths, may be halved in
 * place by FixCarCos — read at runtime) */
static int AntFarmCarSize(CAR_DATA* cp)
{
	if (cp->ap.carCos)
		return cp->ap.carCos->colBox.vz;

	return 300;
}

static void AntFarmCarFraming(CAR_DATA* cp, int* outDist, int* outHeight)
{
	int vz = 300, vy = 120;

	if (cp->ap.carCos)
	{
		vz = cp->ap.carCos->colBox.vz;
		vy = cp->ap.carCos->colBox.vy;
	}

	if (vz < 200)
		vz = 200;

	if (vy < 80)
		vy = 80;

	*outDist = vz * 2 + vy + 380;	/* well back from the traffic */
	*outHeight = 170 + vy / 2;	/* well above it */

	if (*outDist < 700)
		*outDist = 700;

	if (*outDist > 1350)
		*outDist = 1350;

	if (*outHeight > 460)
		*outHeight = 460;
}

/* has this style been picked in the last few cuts? */
static int AntFarmStyleRecent(int style)
{
	int i;

	for (i = 0; i < s.recentCount; i++)
		if (s.recentStyles[i] == style)
			return 1;

	return 0;
}

static void AntFarmRememberStyle(int style)
{
	int i;

	for (i = ANTFARM_STYLE_MEMORY - 1; i > 0; i--)
		s.recentStyles[i] = s.recentStyles[i - 1];

	s.recentStyles[0] = style;

	if (s.recentCount < ANTFARM_STYLE_MEMORY)
		s.recentCount++;
}

/* Weighted style pick with a director's memory: a style seen in the last
 * few cuts is heavily de-weighted (not forbidden, or a two-style config
 * could stall). carOnly: 1 = car-capable only, 0 = road-capable only,
 * -1 = any. Overhead/Orbit fit both. */
static int AntFarmPickStyle(int carOnly)
{
	int total = 0, i, roll;
	int weight[ANTFARM_STYLE_COUNT];

	for (i = 0; i < ANTFARM_STYLE_COUNT; i++)
	{
		const ANT_STYLE_DEF* d = &antStyleDefs[i];

		weight[i] = 0;

		if (!s.stylesEnabled[i])
			continue;

		if (!AntFarmStyleImplemented(d->model))
			continue;

		if (carOnly == 1 && d->roadOnly)
			continue;

		if (carOnly == 0 && d->carFirst)
			continue;

		weight[i] = d->weight;

		if (AntFarmStyleRecent(i))
			weight[i] /= 8;

		if (weight[i] < 1)
			weight[i] = 1;

		total += weight[i];
	}

	if (total <= 0)
		return ANTFARM_STYLE_TRIPOD;

	roll = AntRand() % total;

	for (i = 0; i < ANTFARM_STYLE_COUNT; i++)
	{
		if (weight[i] <= 0)
			continue;

		roll -= weight[i];

		if (roll < 0)
			return i;
	}

	return ANTFARM_STYLE_TRIPOD;
}

/* randomize the framing of a freshly picked shot so no two cuts look alike */
static void AntFarmInitShotVars(void)
{
	s.shotSideSign = (AntRand() & 1) ? 1 : -1;
	s.shotMargin = AntRandRange(180, 360);		/* clear of the kerb */
	s.shotLookAhead = AntRandRange(800, 1100);	/* stable look-ahead */
	s.shotOrbitAmp = 0;
	s.armFrac = 256;
	s.shotOrbitPhase = AntRand() & 4095;

	{
		const ANT_STYLE_DEF* d = &antStyleDefs[s.style];

		s.shotHeight = AntRandRange(d->heightLo, d->heightHi);
		s.shotFwd = AntRandRange(d->fwdLo, d->fwdHi);
		s.shotSide = AntRandRange(d->sideLo, d->sideHi);

		if (d->zoom)
		{
			/* a zoom row sits at one end of its lens range and pushes to the
			 * other and back across the shot (see AntFarmFovTarget) */
			s.shotScrZ = d->scrZLo;
			s.shotZoomTo = d->scrZHi;
		}
		else
		{
			s.shotScrZ = AntRandRange(d->scrZLo, d->scrZHi);
			s.shotZoomTo = s.shotScrZ;
		}

		/* some rows slowly pan; the rest hold still */
		if (d->orbitHi > 0 && AntRandChance(55))
			s.shotOrbitAmp = AntRandRange(d->orbitLo, d->orbitHi);

		/* the row decides how far ahead to look: a short aim watches traffic
		 * cross the lens, a long one looks down the road */
		if (d->across)
			s.shotLookAhead = AntRandRange(0, 220);
		else if (d->aimHi > 0)
			s.shotLookAhead = AntRandRange(d->aimLo, d->aimHi);
	}

	/* safety clamp: a long lens is fine, a fisheye is not */
	if (s.shotScrZ < 200)
		s.shotScrZ = 200;
	if (s.shotScrZ > 360)
		s.shotScrZ = 360;

	if (s.shotZoomTo < 200)
		s.shotZoomTo = 200;
	if (s.shotZoomTo > 360)
		s.shotZoomTo = 360;
}

/* A shot's visible time: the cut interval scaled by how interesting the
 * scene is (long vistas dwell, ephemeral traffic does not), so the pace
 * breathes instead of ticking metronome-steady. */
static int AntFarmComputeDwell(void)
{
	const ANT_STYLE_DEF* d = &antStyleDefs[s.style];
	int base = AntIntervalMs();
	int mul = d->dwell;
	int len = 0;

	if (s.targetKind == ANTFARM_TARGET_ROAD &&
		s.roadSurfId >= 0 && s.roadSurfId < NumDriver2Straights)
		len = Driver2StraightsPtr[s.roadSurfId].length;

	if (len > 3000)
		mul = mul * 130 / 100;
	else if (len > 0 && len < 900)
		mul = mul * 80 / 100;

	/* a car subject is transient - do not linger on an empty frame */
	if (s.targetKind == ANTFARM_TARGET_CAR)
		mul = mul * 70 / 100;

	if (mul < ANTFARM_DWELL_MIN)
		mul = ANTFARM_DWELL_MIN;

	if (mul > ANTFARM_DWELL_MAX)
		mul = ANTFARM_DWELL_MAX;

	return base * mul / 100;
}

/* The lens target for this frame. A zoom row sits at one end of its lens range
 * and pushes to the other and back across the shot; everything else holds the
 * lens it was given. This feeds the same smoothing as the rest of the FOV
 * handling, so the movement is a slow breathe rather than a jump. */
static int AntFarmFovTarget(void)
{
	const ANT_STYLE_DEF* d = &antStyleDefs[s.style];
	unsigned long shotMs;
	unsigned long elapsed;
	int t, factor;

	if (!d->zoom || s.shotZoomTo == s.shotScrZ)
		return s.shotScrZ;

	shotMs = (unsigned long)(s.dwellMs > 0 ? s.dwellMs : AntIntervalMs());
	elapsed = AntTicks() - s.shotStart;

	t = (elapsed >= shotMs) ? 1000 : (int)(elapsed * 1000 / shotMs);

	/* half a sine: 0 at both ends of the shot, 4096 (= 1.0) in the middle */
	factor = RSIN((t * 2048) / 1000);

	return s.shotScrZ + (int)(((long)(s.shotZoomTo - s.shotScrZ) * factor) / 4096);
}

/* ------------------------------------------------------------------ */
/* Car mode cycling                                                   */
/* ------------------------------------------------------------------ */

/* Pick a new car mode, avoiding repetition if possible */
static int AntFarmPickCarMode(void)
{
	int modes[CAR_MODE_COUNT];
	int num = 0;
	int i;

	/* Build list of available modes (exclude static if road not suitable?) */
	for (i = 0; i < CAR_MODE_COUNT; i++) {
		modes[num++] = i;
	}

	/* If we have a previous mode, try to avoid it */
	int prev = s.carMode;
	int attempts = 0;
	int pick;
	while (attempts < 20) {
		pick = modes[AntRand() % num];
		if (pick != prev || num == 1)
			break;
		attempts++;
	}
	return pick;
}

/* Compute desired camera position for a car mode */
static void AntFarmComputeCarMode(CAR_DATA* cp, const VECTOR* carPos, int dir, int h,
	VECTOR* desired, int* outLerp)
{
	int dist, height, sideOff;
	int baseDist, baseHeight;
	VECTOR offset;

	/* Get base framing distances for this car */
	int vz = 300, vy = 120;
	if (cp->ap.carCos) {
		vz = cp->ap.carCos->colBox.vz;
		vy = cp->ap.carCos->colBox.vy;
	}
	if (vz < 200) vz = 200;
	if (vy < 80) vy = 80;
	baseDist = vz * 2 + vy + 380;
	if (baseDist < 700) baseDist = 700;
	if (baseDist > 1350) baseDist = 1350;
	baseHeight = 170 + vy / 2;
	if (baseHeight > 460) baseHeight = 460;

	/* Mode-specific offsets */
	switch (s.carMode)
	{
	case CAR_MODE_CHASE_BEHIND:
		/* Classic chase: behind and above */
		dist = baseDist;
		height = baseHeight;
		offset.vx = FIXEDH(RSIN((dir + 2048) & 0xfff) * dist);
		offset.vz = FIXEDH(RCOS((dir + 2048) & 0xfff) * dist);
		offset.vy = -h - height;
		*outLerp = 8;
		break;

	case CAR_MODE_CHASE_FRONT:
		/* Forward-looking: in front of the car */
		dist = baseDist;
		height = baseHeight;
		offset.vx = FIXEDH(RSIN(dir) * dist);
		offset.vz = FIXEDH(RCOS(dir) * dist);
		offset.vy = -h - height;
		*outLerp = 8;
		break;

	case CAR_MODE_SIDE_LEFT:
		/* Left side, slightly elevated */
		dist = baseDist * 2 / 3;
		height = baseHeight * 3 / 4;
		sideOff = dist;
		offset.vx = FIXEDH(RSIN((dir + 1024) & 0xfff) * sideOff);
		offset.vz = FIXEDH(RCOS((dir + 1024) & 0xfff) * sideOff);
		offset.vy = -h - height;
		*outLerp = 7;
		break;

	case CAR_MODE_SIDE_RIGHT:
		/* Right side, slightly elevated */
		dist = baseDist * 2 / 3;
		height = baseHeight * 3 / 4;
		sideOff = dist;
		offset.vx = FIXEDH(RSIN((dir + 3072) & 0xfff) * sideOff);
		offset.vz = FIXEDH(RCOS((dir + 3072) & 0xfff) * sideOff);
		offset.vy = -h - height;
		*outLerp = 7;
		break;

	case CAR_MODE_OVERHEAD:
		/* High overhead, close to car */
		dist = 400 + vz;
		height = 550 + vy;
		if (dist > 600) dist = 600;
		if (height > 720) height = 720;
		offset.vx = FIXEDH(RSIN((dir + 2048) & 0xfff) * dist);
		offset.vz = FIXEDH(RCOS((dir + 2048) & 0xfff) * dist);
		offset.vy = -h - height;
		*outLerp = 9;
		break;

	case CAR_MODE_STATIC:
	default:
		/* Static roadside: place a fixed camera, use existing static track logic */
	{
		VECTOR staticPos;
		AntFarmStaticTrack(cp, &staticPos);
		desired->vx = staticPos.vx;
		desired->vy = staticPos.vy;
		desired->vz = staticPos.vz;
		*outLerp = 4;
		return;
	}
	}

	desired->vx = carPos->vx + offset.vx;
	desired->vy = offset.vy;
	desired->vz = carPos->vz + offset.vz;
}

/* ------------------------------------------------------------------ */
/* shot planning (called during the black CUT)                        */
/* ------------------------------------------------------------------ */

/* point the shot's road reference at s.roadSurfId (the area road): pick a
 * lane + distance and set the spool anchor. Sampling near a straight's END
 * frames junctions (junctions have no centre of their own). */
static void AntFarmSetupRoadShot(void)
{
	const ANT_STYLE_DEF* d = &antStyleDefs[s.style];
	DRIVER2_STRAIGHT* rd;
	int nLanes;
	int len;

	AntFarmEnsureRoadCache();

	if (s.roadSurfId < 0 || s.roadSurfId >= NumDriver2Straights)
	{
		s.targetPos = s.areaPos;
		return;
	}

	rd = &Driver2StraightsPtr[s.roadSurfId];
	len = rd->length;

	nLanes = ROAD_WIDTH_IN_LANES(rd);
	s.roadLane = (nLanes > 0) ? (AntRand() % nLanes) : 0;

	if (d->model == ANT_MODEL_DOLLY || d->model == ANT_MODEL_CRANE)
	{
		/* Start the dolly at a negative offset so it begins before the road start,
		 * and aim further ahead to avoid the camera pointing straight down at the end. */
		int offset = 800 + (AntRand() % 200);

		s.roadDist = -offset;	/* start before the road */
		s.junctionCorner = 0;
		s.shotLookAhead = 1000 + (AntRand() % 400);
	}
	else if (AntRandChance(40))
	{
		/* roadside: sometimes frame a road END, where junctions cluster */
		int endShot = (AntRand() & 1) ? 1 : -1;	/* 1 far end, -1 near end */

		s.roadDist = (endShot > 0) ? (len - 300 - (AntRand() % 500))
			: (300 + (AntRand() % 500));

		if (AntRandChance(45))
		{
			s.roadDist = (endShot > 0) ? (len + 260 + (AntRand() % 320))
				: (-260 - (AntRand() % 320));
			s.junctionCorner = 1;
		}
		else
		{
			s.junctionCorner = 0;
		}
	}
	else
	{
		s.roadDist = len / 2 + (AntRand() % (len / 4)) - len / 8;
		s.junctionCorner = 0;
	}

	/* corner shots may sit just past either end of the road */
	if (s.roadDist < -150)
		s.roadDist = -150;

	if (s.roadDist > len + 150)
		s.roadDist = len + 150;

	{
		int x, z, heading;

		if (AntFarmShotRoadPoint(s.roadSurfId, s.roadDist, s.roadLane, &x, &z, &heading))
		{
			s.targetPos.vx = x;
			s.targetPos.vy = AntFarmMapHeight(x, z);
			s.targetPos.vz = z;
		}
		else
		{
			s.targetPos = s.areaPos;
		}
	}
}

/* Helper to pick style and target kind */
static void AntFarmPickStyleAndTarget(void)
{
	s.style = AntFarmPickStyle(-1);
	AntFarmRememberStyle(s.style);

	if (antStyleDefs[s.style].carFirst) {
		s.targetKind = ANTFARM_TARGET_CAR;
	}
	else if (!antStyleDefs[s.style].roadOnly) {
		/* overhead/orbit can watch either */
		s.targetKind = (AntRand() & 1) ? ANTFARM_TARGET_CAR : ANTFARM_TARGET_ROAD;
	}
	else {
		s.targetKind = ANTFARM_TARGET_ROAD;
	}
}

/* Plan the shot: for road targets, pick a road near the area and set up the shot.
 * For car targets, just mark it – the car will be picked later. */
static void AntFarmPlanShot(void)
{
	s.trackPlaced = 0;
	s.targetCarId = -1;

	AntFarmEnsureRoadCache();

	if (s.targetKind == ANTFARM_TARGET_ROAD) {
		int idx = -1;

		/* a waterfront row asks for a road that really runs beside water */
		if (antStyleDefs[s.style].nearWater)
			idx = AntFarmPickWaterRoad(s.areaPos.vx, s.areaPos.vz);

		if (idx < 0)
			idx = AntFarmPickRoadNear(s.areaPos.vx, s.areaPos.vz);

		if (idx >= 0) {
			if ((antStyleDefs[s.style].model == ANT_MODEL_DOLLY ||
				antStyleDefs[s.style].model == ANT_MODEL_CRANE) &&
				Driver2StraightsPtr[idx].length < 1600) {
				int better = AntFarmPickSurface(1600);
				if (better >= 0) idx = better;
			}
			s.roadSurfId = idx;
			AntFarmSetupRoadShot();
		}
		else {
			/* no road found - fall back to a safe hover above areaPos */
			s.roadSurfId = -1;
			s.targetPos = s.areaPos;
		}
	}
	else {
		s.targetPos = s.areaPos;
	}
	AntFarmInitShotVars();

	/* If we have a car target, initialize car mode */
	if (s.targetKind == ANTFARM_TARGET_CAR) {
		s.carMode = CAR_MODE_CHASE_BEHIND; /* initial mode */
		s.carModeIndex = 0;
		s.carModeStart = AntTicks();
	}
}

/* ------------------------------------------------------------------ */
/* adopt the engine's debug -seed so a run is reproducible when asked */
static int AntFarmOnGameStart(void* userdata, void* args)
{
	JER_ARGS_GAME_START* a = (JER_ARGS_GAME_START*)args;

	(void)userdata;

	if (a != NULL && a->seed != 0)
		AntFarmSetRunSeed((unsigned int)a->seed);

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* rogue-car (lead AI) events                                         */
/* ------------------------------------------------------------------ */

static int AntFarmLeadTotaled(void)
{
	CAR_DATA* cp;

	if (s.targetCarId < 0 || s.targetCarId >= MAX_CARS)
		return 1;

	cp = &car_data[s.targetCarId];

	if (cp->controlType != CONTROL_TYPE_LEAD_AI)
		return 1;

	if (cp->totalDamage >= ANTFARM_LEAD_TOTAL)
		return 1;

	return 0;
}

/* the rogue car left the playable world (drove off the map or fell into
 * the void) — the event must end immediately, before the camera or the
 * pinned player car follow it out */
static int AntFarmLeadEscaped(void)
{
	CAR_DATA* cp;
	int maxX, maxZ;

	if (s.targetCarId < 0 || s.targetCarId >= MAX_CARS)
		return 0;

	cp = &car_data[s.targetCarId];

	maxX = units_across_halved - 3000;
	maxZ = units_down_halved - 3000;

	if (cp->hd.where.t[0] < -maxX || cp->hd.where.t[0] > maxX ||
		cp->hd.where.t[2] < -maxZ || cp->hd.where.t[2] > maxZ)
		return 1;

	if (cp->hd.where.t[1] < -6000 || cp->hd.where.t[1] > 8000)
		return 1;

	return 0;
}

static void AntFarmStartLead(void)
{
	int newSlot = -1, i;
	CAR_DATA* src = &car_data[s.targetCarId];
	LONGVECTOR4 pos;

	for (i = 0; i < MAX_CARS; i++)
	{
		if (car_data[i].controlType == CONTROL_TYPE_NONE && reservedSlots[i] == 0)
		{
			newSlot = i;
			break;
		}
	}

	if (newSlot < 0)
	{
		s.ctx->jer_log(s.ctx, "[antfarm] no free slot — rogue event skipped\n");
		return;
	}

	pos[0] = src->hd.where.t[0];
	pos[1] = src->hd.where.t[1];
	pos[2] = src->hd.where.t[2];
	pos[3] = 0;

	InitCar(&car_data[newSlot], src->hd.direction & 0xfff, &pos,
		CONTROL_TYPE_LEAD_AI, src->ap.model, src->ap.palette & 255, NULL);

	src->controlType = CONTROL_TYPE_NONE;

	s.targetCarId = newSlot;
	s.leadMode = 1;
	s.leadEnding = 0;
	s.style = ANTFARM_STYLE_CHASE;
	s.targetKind = ANTFARM_TARGET_CAR;
	s.trackPlaced = 0;
	s.carMode = CAR_MODE_CHASE_BEHIND;
	s.carModeIndex = 0;
	s.carModeStart = AntTicks();
	AntFarmInitShotVars();

	CopsAllowed = 1;
	*GetPlayerFelony(&MainPlayer) = 2500;

	s.ctx->jer_log(s.ctx, "[antfarm] rogue car went rogue — cops engaged\n");
}

static void AntFarmEndLead(void)
{
	CopsAllowed = s.savedCopsAllowed;
	*GetPlayerFelony(&MainPlayer) = (short)s.savedPlayerFelony;

	if (s.targetCarId >= 0 && s.targetCarId < MAX_CARS)
		car_data[s.targetCarId].controlType = CONTROL_TYPE_NONE;

	s.targetCarId = -1;
	s.leadMode = 0;
	s.leadEnding = 0;

	s.ctx->jer_log(s.ctx, "[antfarm] rogue car ended — back to touring\n");
}

/* ------------------------------------------------------------------ */
/* camera                                                             */
/* ------------------------------------------------------------------ */

/* STATIC style: a fixed roadside camera the target car drives past. */
static void AntFarmStaticTrack(CAR_DATA* cp, VECTOR* desired)
{
	int carX = cp->hd.where.t[0];
	int carZ = cp->hd.where.t[2];
	int dir = cp->hd.direction;
	int dx = carX - s.trackCamPos.vx;
	int dz = carZ - s.trackCamPos.vz;
	int passed = FIXEDH(dx * RSIN(dir) + dz * RCOS(dir)) < 0;
	int far = dx * dx + dz * dz > 3200 * 2200;

	if (!s.trackPlaced || far)
	{
		int side = (AntRand() & 1) ? 1024 : 3072;
		int spotDist = 1000 + (AntRand() % 400);
		int sideDist = 650 + (AntRand() % 450);
		int hgt = 130 + (AntRand() % 90);

		s.trackCamPos.vx = carX + FIXEDH(RSIN((dir + 2048) & 0xfff) * spotDist);
		s.trackCamPos.vz = carZ + FIXEDH(RCOS((dir + 2048) & 0xfff) * spotDist);
		s.trackCamPos.vy = -(AntFarmMapHeight(s.trackCamPos.vx, s.trackCamPos.vz) + hgt);

		s.trackCamPos.vx += FIXEDH(RSIN((dir + side) & 0xfff) * sideDist);
		s.trackCamPos.vz += FIXEDH(RCOS((dir + side) & 0xfff) * sideDist);

		s.trackPlaced = 1;
	}

	*desired = s.trackCamPos;
}

/* Improved camera position finder: tries to find a clear line-of-sight
 * position near the desired, using multiple candidate offsets. */
static int AntFarmFindClearCamera(const VECTOR* aim, const VECTOR* desired, VECTOR* outCam)
{
	VECTOR candidates[32];
	int numCandidates = 0;
	VECTOR delta;
	int dist, dx, dz;
	int i;

	/* compute vector from aim to desired */
	delta.vx = desired->vx - aim->vx;
	delta.vy = desired->vy - aim->vy;
	delta.vz = desired->vz - aim->vz;
	dist = sqrt(delta.vx * delta.vx + delta.vy * delta.vy + delta.vz * delta.vz);
	if (dist < 1) dist = 1;

	/* 1) Try the desired position itself */
	candidates[numCandidates++] = *desired;

	/* 2) Pull back fractions: 0.9, 0.8, ..., 0.1 */
	for (int frac = 9; frac >= 1; frac--) {
		VECTOR pos;
		float t = frac / 10.0f;
		pos.vx = aim->vx + (int)(delta.vx * t);
		pos.vy = aim->vy + (int)(delta.vy * t);
		pos.vz = aim->vz + (int)(delta.vz * t);
		candidates[numCandidates++] = pos;
		if (numCandidates >= 32) break;
	}

	/* 3) Rotate around aim: try different azimuth angles (0, 45, 90, ...) at 80% and 120% distance */
	for (int ang = 0; ang < 360; ang += 45) {
		float rad = ang * 3.14159f / 180.0f;
		float cosA = cos(rad);
		float sinA = sin(rad);
		/* rotate the delta vector around the Y axis */
		int rx = (int)(delta.vx * cosA - delta.vz * sinA);
		int rz = (int)(delta.vx * sinA + delta.vz * cosA);
		/* two distances: 0.8 and 1.2 of original */
		for (int dscale = 8; dscale <= 12; dscale += 4) {
			VECTOR pos;
			pos.vx = aim->vx + (rx * dscale) / 10;
			pos.vy = aim->vy + (delta.vy * dscale) / 10;  /* keep same vertical ratio */
			pos.vz = aim->vz + (rz * dscale) / 10;
			candidates[numCandidates++] = pos;
			if (numCandidates >= 32) break;
		}
		if (numCandidates >= 32) break;
	}

	/* 4) Try higher/lower positions */
	for (int hoff = -200; hoff <= 200; hoff += 100) {
		VECTOR pos = *desired;
		pos.vy += hoff;
		candidates[numCandidates++] = pos;
		if (numCandidates >= 32) break;
	}

	/* Now test each candidate */
	for (i = 0; i < numCandidates; i++) {
		VECTOR cam = candidates[i];
		VECTOR camWorld = cam;
		VECTOR aimWorld = *aim;
		camWorld.vy = -camWorld.vy;
		aimWorld.vy = -aimWorld.vy;

		/* Clamp above ground */
		AntFarmClampAboveGround(&cam);

		/* Check line-of-sight */
		if (lineClear(&camWorld, &aimWorld) == 0)
			continue;

		/* Additional check: ensure camera is not too low or too high */
		int ground = -AntFarmMapHeight(cam.vx, cam.vz);
		if (cam.vy > ground - 40)  /* at least 40 units above ground */
			continue;

		/* Also ensure the camera is not inside geometry by using the collider */
		CAR_DATA* jcam = &car_data[CAMERA_COLLIDER_CARID];
		ClearMem((char*)jcam, sizeof(CAR_DATA));
		jcam->controlType = CONTROL_TYPE_CAMERACOLLIDER;
		jcam->hd.direction = s.camAngle.vy & 0xfff;

		jcam->hd.where.t[0] = cam.vx;
		jcam->hd.where.t[1] = -cam.vy;
		jcam->hd.where.t[2] = cam.vz;

		jcam->hd.oBox.location.vx = cam.vx;
		jcam->hd.oBox.location.vy = -cam.vy;
		jcam->hd.oBox.location.vz = cam.vz;

		CheckScenaryCollisions(jcam);

		cam.vx = jcam->hd.where.t[0];
		cam.vy = -jcam->hd.where.t[1];
		cam.vz = jcam->hd.where.t[2];

		/* Clamp after collider */
		AntFarmClampAboveGround(&cam);

		/* Re-check LOS after collider */
		camWorld = cam;
		camWorld.vy = -camWorld.vy;
		if (lineClear(&camWorld, &aimWorld) == 0)
			continue;

		/* Valid position found */
		*outCam = cam;
		return 1;
	}

	/* Fallback: just use a position directly above the aim at a safe height */
	{
		int ground = -AntFarmMapHeight(aim->vx, aim->vz);
		outCam->vx = aim->vx;
		outCam->vz = aim->vz;
		outCam->vy = ground - 150;
		return 1;
	}
}

static void AntFarmComputeCamera(VECTOR* outPos, SVECTOR* outAngle)
{
	const ANT_STYLE_DEF* d = &antStyleDefs[s.style];
	VECTOR desired = { 0, 0, 0 };
	VECTOR aim = { 0, 0, 0 };
	unsigned long now = AntTicks();
	int lerp;

	if (s.targetKind == ANTFARM_TARGET_CAR)
	{
		CAR_DATA* cp = AntFarmValidCar();
		VECTOR carPos;
		int dir, h;

		if (cp == NULL)
		{
			*outPos = s.camPos;
			*outAngle = s.camAngle;
			return;
		}

		carPos.vx = cp->hd.where.t[0];
		carPos.vy = cp->hd.where.t[1];
		carPos.vz = cp->hd.where.t[2];

		dir = cp->hd.direction;
		h = AntFarmMapHeight(carPos.vx, carPos.vz);

		if (d->model == ANT_MODEL_ATTACH)
		{
			/* Rig on the car. Offsets are in the car's own frame - forward along
			 * its heading, then to its right - so the rig rides the body. Yaw
			 * only (no roll/pitch), which keeps the horizon level and the shot
			 * watchable rather than nauseating. `behind` sits a rig at the tail,
			 * and the per-shot side sign picks which flank. */
			int fwd = s.shotFwd * (d->behind ? -1 : 1);
			int side = s.shotSide * s.shotSideSign;
			int right = (dir + 3072) & 0xfff;

			/* Scale the rig to the car's own body, so a bus and a sports car
			 * both get a camera that sits just outside the panels. The nominal
			 * body is colBox.vz=300 / vx=130 (see AntFarmCarSize). */
			if (cp->ap.carCos)
			{
				int vz = cp->ap.carCos->colBox.vz;
				int vx = cp->ap.carCos->colBox.vx;
				int scale;

				if (vz < 200) vz = 200;
				if (vx < 80) vx = 80;

				scale = vz * 100 / 300;
				if (scale < 60) scale = 60;
				if (scale > 200) scale = 200;
				fwd = fwd * scale / 100;

				scale = vx * 100 / 130;
				if (scale < 60) scale = 60;
				if (scale > 220) scale = 220;
				side = side * scale / 100;
			}

			desired.vx = carPos.vx + FIXEDH(RSIN(dir) * fwd) + FIXEDH(RSIN(right) * side);
			desired.vz = carPos.vz + FIXEDH(RCOS(dir) * fwd) + FIXEDH(RCOS(right) * side);
			desired.vy = -(h + s.shotHeight);
			lerp = 100 / d->settle;

			/* look a little way ahead of the car rather than at it, so the
			 * car sits in frame and the road reads beyond it */
			aim.vx = carPos.vx + FIXEDH(RSIN(dir) * s.shotLookAhead);
			aim.vz = carPos.vz + FIXEDH(RCOS(dir) * s.shotLookAhead);
			aim.vy = -(carPos.vy + 40);
		}
		else if (d->model == ANT_MODEL_TRIPODZ)
		{
			/* A fixed vantage, set up from the car when the shot begins and then
			 * left alone: the car drives through the frame while the lens pushes
			 * in and back out (the row's zoom) and the camera just follows it in
			 * yaw. That yaw tracking is the "pan" a tripod would do. */
			if (!s.vantageSet)
			{
				int right = (dir + 3072) & 0xfff;
				int fwd = s.shotFwd;
				int side = s.shotSide * s.shotSideSign;

				s.shotVantage.vx = carPos.vx + FIXEDH(RSIN(dir) * fwd) + FIXEDH(RSIN(right) * side);
				s.shotVantage.vz = carPos.vz + FIXEDH(RCOS(dir) * fwd) + FIXEDH(RCOS(right) * side);
				s.shotVantage.vy = -AntFarmMapHeight(s.shotVantage.vx, s.shotVantage.vz)
					- s.shotHeight;
				s.vantageSet = 1;
			}

			desired = s.shotVantage;
			lerp = 100 / d->settle;

			aim = carPos;
			aim.vy = -(carPos.vy + 30);
		}
		else if (d->model == ANT_MODEL_TRACK)
		{
			/* fixed roadside camera the car drives past */
			AntFarmStaticTrack(cp, &desired);
			lerp = 100 / d->settle;

			aim = carPos;
			aim.vy = -(carPos.vy + 50);
		}
		else
		{
			/* Use car mode to compute desired position */
			AntFarmComputeCarMode(cp, &carPos, dir, h, &desired, &lerp);

			/* ORBIT: circle the car slowly instead of using the chase angles */
			if (d->model == ANT_MODEL_ORBIT)
			{
				int radius = 900 + s.shotHeight;
				int ang = (s.shotOrbitPhase + (int)(((now - s.shotStart)
					% ANTFARM_ORBIT_PERIOD_MS) * 4096 / ANTFARM_ORBIT_PERIOD_MS)) & 4095;

				desired.vx = carPos.vx + FIXEDH(RSIN(ang) * radius);
				desired.vz = carPos.vz + FIXEDH(RCOS(ang) * radius);
				desired.vy = -(h + s.shotHeight);
				lerp = 100 / antStyleDefs[s.style].settle;
			}

			aim = carPos;
			aim.vy = -(carPos.vy + 50);
		}

		s.spool = carPos;
		s.targetPos = carPos;

		/* if static mode, lerp slower */
		if (s.carMode == CAR_MODE_STATIC) lerp = 4;
	}
	else	/* ANTFARM_TARGET_ROAD */
	{
		VECTOR roadPt = { 0, 0, 0 };
		int heading = 0;

		if (s.roadSurfId < 0 || s.roadSurfId >= NumDriver2Straights) {
			/* fallback: hover above areaPos */
			desired = s.areaPos;
			desired.vy = -AntFarmMapHeight(desired.vx, desired.vz) - s.shotHeight;
			aim = s.areaPos;
			aim.vy = desired.vy + 100;
			lerp = 100 / antStyleDefs[s.style].settle;
		}
		else {
			if (AntFarmShotRoadRender(s.roadDist, &roadPt, &heading) == 0)
			{
				*outPos = s.camPos;
				*outAngle = s.camAngle;
				return;
			}

			if (d->model == ANT_MODEL_ROADSIDE)
			{
				DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[s.roadSurfId];
				int count = ROAD_LANES_COUNT(rd);
				int sideShift = count * 128 - (s.roadLane * 128);
				int dirBit = ROAD_LANE_DIR(rd, s.roadLane);
				int laneSideFromC = sideShift * ((dirBit == 0) ? 1 : -1);
				int camOffFromCentre = s.shotSideSign * (count * 128 + s.shotMargin);
				int side = (heading + (s.shotSideSign > 0 ? 1024 : 3072)) & 0xfff;
				int off = camOffFromCentre - laneSideFromC;
				int sway = RSIN((now / 200) & 4095) >> 9;
				int bobY = RSIN((now / 130) & 4095) >> 10;

				/* a style with an orbit amplitude pans slowly on its fixed side;
				 * its distance from the road is the table's sideLo/Hi */
				if (s.shotOrbitAmp > 0)
				{
					int sweepPhase = (s.shotOrbitPhase + now / 100) & 4095;
					int sweep = FIXEDH(RSIN(sweepPhase) * s.shotOrbitAmp);

					side = (side + 4096 + sweep) & 0xfff;
				}

				desired.vx = roadPt.vx + FIXEDH(RSIN(side) * off) + sway;
				desired.vy = roadPt.vy - s.shotHeight + bobY;
				desired.vz = roadPt.vz + FIXEDH(RCOS(side) * off);

				int aimDist = s.junctionCorner ? (s.roadDist - s.shotLookAhead)
					: (s.roadDist + s.shotLookAhead);

				if (aimDist < 0)
					aimDist = 0;

				if (aimDist > Driver2StraightsPtr[s.roadSurfId].length)
					aimDist = Driver2StraightsPtr[s.roadSurfId].length;

				if (AntFarmShotRoadRender(aimDist, &aim, &heading) == 0)
				{
					aim = roadPt;
				}

				aim.vy = aim.vy - 12;
			}
			else if (d->model == ANT_MODEL_ORBIT)
			{
				/* slow circle around a point on the road */
				int radius = 900 + s.shotHeight;
				int ang = (s.shotOrbitPhase + (int)(((now - s.shotStart)
					% ANTFARM_ORBIT_PERIOD_MS) * 4096 / ANTFARM_ORBIT_PERIOD_MS)) & 4095;

				desired.vx = roadPt.vx + FIXEDH(RSIN(ang) * radius);
				desired.vz = roadPt.vz + FIXEDH(RCOS(ang) * radius);
				desired.vy = roadPt.vy - s.shotHeight;

				aim = roadPt;
				aim.vy = roadPt.vy - 20;
			}
			else if (d->model == ANT_MODEL_CRANE)
			{
				/* slow rise from road level, revealing the street ahead */
				VECTOR startPt = { 0, 0, 0 };
				VECTOR endPt = { 0, 0, 0 };
				VECTOR aimPt = { 0, 0, 0 };
				int len = Driver2StraightsPtr[s.roadSurfId].length;
				int dummyHeading = 0;
				int t, tt, curDist, aimDist;
				unsigned long shotMs = (unsigned long)(s.dwellMs > 0 ? s.dwellMs : AntIntervalMs());
				unsigned long shotElapsed = now - s.shotStart;
				int startD = s.roadDist;
				int endD = len + 200;
				int curH;

				t = (shotElapsed >= shotMs) ? 1000
					: (int)(shotElapsed * 1000 / shotMs);

				tt = t * t * (3000 - 2 * t) / 1000000;

				AntFarmShotRoadRender(startD, &startPt, &dummyHeading);
				AntFarmShotRoadRender(endD, &endPt, &dummyHeading);

				desired.vx = startPt.vx + (endPt.vx - startPt.vx) * tt / 1000;
				desired.vz = startPt.vz + (endPt.vz - startPt.vz) * tt / 1000;

				curH = ANTFARM_CRANE_LOW + (s.shotHeight - ANTFARM_CRANE_LOW) * tt / 1000;
				desired.vy = -AntFarmMapHeight(desired.vx, desired.vz) - curH;

				curDist = startD + (endD - startD) * tt / 1000;
				aimDist = curDist + s.shotLookAhead;
				if (aimDist > endD) aimDist = endD;

				AntFarmShotRoadRender(aimDist, &aimPt, &dummyHeading);
				aim = aimPt;
				aim.vy = aimPt.vy - 40;
			}
			else	/* ANT_MODEL_DOLLY */
			{
				VECTOR startPt = { 0, 0, 0 };
				VECTOR endPt = { 0, 0, 0 };
				VECTOR aimPt = { 0, 0, 0 };
				int len = Driver2StraightsPtr[s.roadSurfId].length;
				int dummyHeading = 0;
				int t, tt, curDist, aimDist;
				unsigned long shotElapsed = now - s.shotStart;

				int startD = s.roadDist;
				int endD = len + 300 + (AntRand() % 200);

				{
					unsigned long shotMs = (unsigned long)(s.dwellMs > 0 ? s.dwellMs : AntIntervalMs());

					t = (shotElapsed >= shotMs) ? 1000
						: (int)(shotElapsed * 1000 / shotMs);
				}

				/* smoothstep: monotonic 0..1000 (the old 1000-2t form went
				 * negative past the midpoint, stalling and reversing the dolly) */
				tt = t * t * (3000 - 2 * t) / 1000000;

				AntFarmShotRoadRender(startD, &startPt, &dummyHeading);
				AntFarmShotRoadRender(endD, &endPt, &dummyHeading);

				desired.vx = startPt.vx + (endPt.vx - startPt.vx) * tt / 1000;
				desired.vz = startPt.vz + (endPt.vz - startPt.vz) * tt / 1000;
				desired.vy = -(AntFarmMapHeight(desired.vx, desired.vz)) - s.shotHeight
					+ (RSIN((now / 11) & 4095) >> 6);

				curDist = startD + (endD - startD) * tt / 1000;
				aimDist = curDist + s.shotLookAhead;
				if (aimDist > endD) aimDist = endD;

				AntFarmShotRoadRender(aimDist, &aimPt, &dummyHeading);
				aim = aimPt;
				aim.vy = aimPt.vy - 60;
			}
		}

		lerp = 100 / antStyleDefs[s.style].settle;
	}

	/* A degenerate pair (aim == camera) gives PointAtTarget no direction at
	 * all, which shows up as the view whipping to a random angle mid-shot. */
	{
		long long adx = (long long)aim.vx - desired.vx;
		long long adz = (long long)aim.vz - desired.vz;

		if (adx * adx + adz * adz < 400 * 400)
		{
			/* push the aim along the current view direction */
			aim.vx += FIXEDH(RSIN(s.camAngle.vy) * 500);
			aim.vz += FIXEDH(RCOS(s.camAngle.vy) * 500);
		}
	}

	s.aimPos = aim;
	/* Use the new clear camera finder to avoid scenery collisions. An attached
	 * rig is deliberately inches off the body, so pulling it back for
	 * line-of-sight would tear it off the car - it keeps its position and only
	 * the ground clamp applies. */
	if (d->model == ANT_MODEL_ATTACH)
	{
		AntFarmClampAboveGround(&desired);
	}
	else
	{
		VECTOR clearCam;

		if (AntFarmFindClearCamera(&aim, &desired, &clearCam))
		{
			desired = clearCam;
		}
		else
		{
			/* fallback: use desired as is, but clamped */
			AntFarmClampAboveGround(&desired);
		}
	}

	if (!s.camSnapped)
	{
		s.camPos = desired;
		s.camSnapped = 1;
	}
	else
	{
		s.camPos.vx += (desired.vx - s.camPos.vx) * lerp / 100;
		s.camPos.vy += (desired.vy - s.camPos.vy) * lerp / 100;
		s.camPos.vz += (desired.vz - s.camPos.vz) * lerp / 100;
	}

	{
		SVECTOR ang;
		PointAtTarget(&s.camPos, &aim, &ang);
		*outPos = s.camPos;
		*outAngle = ang;
	}
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void AntFarmSetActive(int on)
{
	if (!on)
		s.pendingEnable = 0;

	if (on == s.active)
		return;

	if (on)
	{
		if (game_over || gInGameCutsceneActive || quick_replay ||
			NumPlayers != 1 || NoPlayerControl)
		{
			s.ctx->jer_log(s.ctx, "[antfarm] refused: not in a playable single-player state\n");
			return;
		}

		s.savedStopPadReads = gStopPadReads;
		s.savedDoOverlays = gDoOverlays;
		s.savedSpoolXZ = MainPlayer.spoolXZ;
		s.savedCopsAllowed = CopsAllowed;
		s.savedMasterVolume = gMasterVolume;
		s.savedPlayerFelony = *GetPlayerFelony(&MainPlayer);
		s.savedScrZ = scr_z;
		s.savedCameraCar = CameraCar;

		gStopPadReads = 1;
		gDoOverlays = 0;
		CopsAllowed = 0;

		/* mute all SFX for a calm watch (music is separate - gMusicVolume - and
		 * is deliberately left alone). The readme always claimed this; the code
		 * never actually did it. */
		SetMasterVolume(0);

		s.savedPlayerCarControlType = 0;

		if (MainPlayer.playerType == PLAYER_TYPE_CAR &&
			MainPlayer.playerCarId >= 0 && MainPlayer.playerCarId < MAX_CARS)
		{
			CAR_DATA* pcar = &car_data[MainPlayer.playerCarId];

			s.savedPlayerCarControlType = pcar->controlType;
			s.savedPlayerReservedSlot = reservedSlots[MainPlayer.playerCarId];
			s.savedPlayerCarPos[0] = pcar->hd.where.t[0];
			s.savedPlayerCarPos[1] = pcar->hd.where.t[1];
			s.savedPlayerCarPos[2] = pcar->hd.where.t[2];
		}

		s.active = 1;
		s.camSnapped = 0;
		s.fade = 0;
		s.state = ANTFARM_STATE_SHOW;
		s.stateStart = AntTicks();
		s.shotStart = s.stateStart;
		s.cutCount = 0;
		s.leadMode = 0;
		s.leadEnding = 0;
		s.targetCarId = -1;
		s.roadSurfId = -1;
		s.carMode = CAR_MODE_CHASE_BEHIND;
		s.carModeIndex = 0;
		s.carModeStart = AntTicks();

		s.fovCurrent = scr_z;		/* breathe out from the gameplay lens */
		s.recentCount = 0;
		s.recentCarCount = 0;
		s.dwellMs = AntIntervalMs();
		s.holdSince = 0;
		s.voidHolds = 0;
		s.captionUntil = 0;
		s.stillSince = 0;

		s.camPos.vx = camera_position.vx;
		s.camPos.vy = camera_position.vy;
		s.camPos.vz = camera_position.vz;
		s.camAngle = camera_angle;

		s.targetPos.vx = camera_position.vx;
		s.targetPos.vy = camera_position.vy;
		s.targetPos.vz = camera_position.vz;

		AntFarmPickNearArea();
		s.spool = s.areaPos;
		s.targetPos = s.areaPos;

		AntFarmPinPlayerCar();
		MainPlayer.spoolXZ = &s.spool;

		AntFarmPickStyleAndTarget();
		AntFarmPlanShot();

		if (s.targetKind == ANTFARM_TARGET_CAR)
		{
			s.targetCarId = AntFarmPickCarNear(&s.areaPos, 12000,
				(antStyleDefs[s.style].model == ANT_MODEL_ATTACH ||
					antStyleDefs[s.style].model == ANT_MODEL_TRIPODZ) ? 40 : 8);

			if (s.targetCarId >= 0)
				AntFarmRememberCar(s.targetCarId);

			if (s.targetCarId < 0)
			{
				s.targetKind = ANTFARM_TARGET_ROAD;
				s.style = AntFarmPickStyle(0);
				AntFarmPlanShot();
			}
		}

		s.camSnapped = 0;
		s.fade = 255;
		s.state = ANTFARM_STATE_FADE_IN;
		s.stateStart = AntTicks();
		s.shotStart = s.stateStart;

		{
			char styleList[80];
			int n = 0, j;

			styleList[0] = '\0';

			for (j = 0; j < ANTFARM_STYLE_COUNT && n < (int)sizeof(styleList) - 16; j++)
			{
				if (!s.stylesEnabled[j])
					continue;

				n += snprintf(styleList + n, sizeof(styleList) - (size_t)n, "%s%s",
					(n > 0) ? "," : "", antStyleDefs[j].key);
			}

			s.ctx->jer_log(s.ctx,
				"[antfarm] enabled (interval %ds, mode interval %ds, modes/cut %d, styles %s, lead=%d%%)\n",
				AntIntervalMs() / 1000, AntCarModeIntervalMs() / 1000,
				s.carModesPerCut, styleList, s.leadEnabled ? s.leadChance : 0);
		}
	}
	else
	{
		if (s.leadMode)
			AntFarmEndLead();

		gStopPadReads = s.savedStopPadReads;
		gDoOverlays = s.savedDoOverlays;
		CopsAllowed = s.savedCopsAllowed;
		SetMasterVolume(s.savedMasterVolume);

		/* give the projection distance and the tracked car back too - both were
		 * mutated every active frame and never restored */
		SetGeomScreen(scr_z = s.savedScrZ);
		CameraCar = s.savedCameraCar;

		MainPlayer.spoolXZ = s.savedSpoolXZ;

		if (s.savedPlayerCarControlType != 0 &&
			MainPlayer.playerCarId >= 0 && MainPlayer.playerCarId < MAX_CARS)
		{
			CAR_DATA* pcar = &car_data[MainPlayer.playerCarId];

			pcar->controlType = (u_char)s.savedPlayerCarControlType;
			reservedSlots[MainPlayer.playerCarId] = (u_char)s.savedPlayerReservedSlot;
			pcar->hd.where.t[0] = s.savedPlayerCarPos[0];
			pcar->hd.where.t[1] = s.savedPlayerCarPos[1];
			pcar->hd.where.t[2] = s.savedPlayerCarPos[2];
			pcar->st.n.fposition[0] = s.savedPlayerCarPos[0] << 4;
			pcar->st.n.fposition[1] = s.savedPlayerCarPos[1] << 4;
			pcar->st.n.fposition[2] = s.savedPlayerCarPos[2] << 4;
			pcar->st.n.linearVelocity[0] = 0;
			pcar->st.n.linearVelocity[1] = 0;
			pcar->st.n.linearVelocity[2] = 0;
			pcar->st.n.angularVelocity[0] = 0;
			pcar->st.n.angularVelocity[1] = 0;
			pcar->st.n.angularVelocity[2] = 0;
		}

		s.active = 0;
		s.fade = 0;

		s.ctx->jer_log(s.ctx,
			"[antfarm] disabled (cuts: %d; void guards: %d; restored pads=%d overlays=%d cops=%d vol=%d)\n",
			s.cutCount, s.voidHolds, s.savedStopPadReads, s.savedDoOverlays,
			s.savedCopsAllowed, s.savedMasterVolume);
	}
}

static void AntFarmToggle(void)
{
	AntFarmSetActive(s.active ? 0 : 1);
	jer_config_set_bool(ANT_MOD_ID, "enabled", s.active);
}

static void AntFarmCheckF9(void)
{
#ifndef PSX
	const Uint8* keys = SDL_GetKeyboardState(NULL);
	int down = (keys != NULL) && keys[SDL_SCANCODE_F9];

	if (down && !s.f9Down)
		AntFarmToggle();

	s.f9Down = down;
#endif
}

/* ------------------------------------------------------------------ */
/* JERICHO hooks                                                      */
/* ------------------------------------------------------------------ */

static void AntFarmPinPlayerCar(void);

static int AntFarmOnFrame(void* userdata, void* args)
{
	unsigned long now;

	(void)userdata;
	(void)args;

	AntFarmCheckF9();

	now = AntTicks();

	/* A config-enabled screensaver turns itself on as soon as the game is in
	 * a playable single-player state. This used to sit BELOW the !active
	 * early-return below, so it could never run — "enabled = 1" did nothing
	 * and the mode only ever started from F9 / the pause menu. */
	if (s.pendingEnable && !s.active &&
		!game_over && !gInGameCutsceneActive && !quick_replay &&
		NumPlayers == 1 && !NoPlayerControl)
	{
		AntFarmSetActive(1);

		if (s.active)
			s.pendingEnable = 0;
	}

	if (!s.active)
		return JER_RESULT_CONTINUE;

	if (gInGameCutsceneActive || quick_replay || game_over)
	{
		AntFarmSetActive(0);
		return JER_RESULT_CONTINUE;
	}

	if (s.state != ANTFARM_STATE_SHOW && now - s.stateStart >= 30000)
	{
		s.ctx->jer_log(s.ctx, "[antfarm] watchdog: state %d stuck — recovering\n", s.state);
		s.fade = 0;
		s.state = ANTFARM_STATE_SHOW;
		s.stateStart = now;
	}

	if (s.leadMode)
	{
		if (AntFarmLeadEscaped())
		{
			AntFarmEndLead();
			s.state = ANTFARM_STATE_FADE_OUT;
			s.stateStart = now;
			return JER_RESULT_CONTINUE;
		}

		if (AntFarmLeadTotaled())
		{
			if (!s.leadEnding)
			{
				s.leadEnding = 1;
				s.leadEndStart = now;
				s.ctx->jer_log(s.ctx, "[antfarm] rogue car wrecked — holding\n");
			}

			if (now - s.leadEndStart >= ANTFARM_LEAD_END_MS)
			{
				AntFarmEndLead();
				s.state = ANTFARM_STATE_FADE_OUT;
				s.stateStart = now;
			}

			return JER_RESULT_CONTINUE;
		}

		CopsAllowed = 1;
		*GetPlayerFelony(&MainPlayer) = 2500;
	}

	switch (s.state)
	{
	case ANTFARM_STATE_SHOW:
		if (!s.leadMode && s.targetKind == ANTFARM_TARGET_CAR &&
			AntFarmValidCar() == NULL)
		{
			s.state = ANTFARM_STATE_FADE_OUT;
			s.stateStart = now;
			break;
		}

		/* Handle car mode cycling - but only for a car-mode-driven style. A
		 * rig or a long-lens vantage *is* the angle, so rotating through the
		 * car modes mid-shot would fight it. */
		if (!s.leadMode && s.targetKind == ANTFARM_TARGET_CAR &&
			AntFarmValidCar() != NULL &&
			antStyleDefs[s.style].model == ANT_MODEL_FOLLOW)
		{
			if (now - s.carModeStart >= (unsigned long)AntCarModeIntervalMs())
			{
				/* Switch to a new mode */
				s.carMode = AntFarmPickCarMode();
				s.carModeStart = now;
				s.carModeIndex++;
				s.ctx->jer_log(s.ctx, "[antfarm] car mode changed to %d\n", s.carMode);
			}

			/* If we've exceeded the number of modes per cut, force a cut */
			if (s.carModeIndex >= s.carModesPerCut)
			{
				s.state = ANTFARM_STATE_FADE_OUT;
				s.stateStart = now;
				break;
			}
		}

		/* A rig on a parked car (or a long lens pointed at one) is a frozen
		 * frame, which is the one thing a screensaver must not show: if the
		 * subject stops moving, cut away rather than stare at it. */
		if (!s.leadMode && !s.leadEnding)
		{
			int model = antStyleDefs[s.style].model;

			if (model == ANT_MODEL_ATTACH || model == ANT_MODEL_TRIPODZ)
			{
				CAR_DATA* cp = AntFarmValidCar();

				if (cp != NULL && cp->hd.speed > -40 && cp->hd.speed < 40)
				{
					if (s.stillSince == 0)
						s.stillSince = now;

					if (now - s.stillSince >= ANTFARM_STILL_MS)
					{
						s.ctx->jer_log(s.ctx,
							"[antfarm] subject parked - cutting away\n");
						s.stillSince = 0;
						s.state = ANTFARM_STATE_FADE_OUT;
						s.stateStart = now;
						break;
					}
				}
				else
				{
					s.stillSince = 0;
				}
			}
		}

		/* Also obey the normal cut interval */
		if (!s.leadEnding &&
			now - s.stateStart >= (unsigned long)(s.dwellMs > 0 ? s.dwellMs : AntIntervalMs()))
		{
			s.state = ANTFARM_STATE_FADE_OUT;
			s.stateStart = now;
		}
		break;

	case ANTFARM_STATE_FADE_OUT:
		s.fade = (int)((now - s.stateStart) * 255 / ANTFARM_FADE_MS);

		if (s.fade >= 255)
		{
			s.fade = 255;
			s.state = ANTFARM_STATE_CUT;
			s.stateStart = now;
			s.cutInit = 0;
		}
		break;

	case ANTFARM_STATE_CUT:
		if (!s.cutInit)
		{
			s.cutInit = 1;
			s.cutStart = now;
			s.cutEnter = now;
			s.cutWaitForCar = 0;
			s.streamRetries = 0;
			s.streamDone = 0;
			s.shotPlanned = 0;
			s.holdSince = 0;
			s.vantageSet = 0;	/* a fixed vantage is resolved per shot */

			{
				long long dx = (long long)s.areaPos.vx - camera_position.vx;
				long long dz = (long long)s.areaPos.vz - camera_position.vz;

				s.jumpDist2 = dx * dx + dz * dz;
			}

			if (s.leadMode)
			{
				s.style = AntFarmPickStyle(1);
				s.trackPlaced = 0;
				s.carMode = CAR_MODE_CHASE_BEHIND;
				s.carModeIndex = 0;
				s.carModeStart = now;
				AntFarmInitShotVars();
				s.shotPlanned = 1;
			}
			else
			{
				/* Pick a far area that is loaded; if none, it will fallback to current position */
				AntFarmPickFarArea();
				s.spool = s.areaPos;
				s.targetPos = s.areaPos;
				AntFarmPickStyleAndTarget();
				s.cutWaitForCar = (s.targetKind == ANTFARM_TARGET_CAR);
				if (s.targetKind == ANTFARM_TARGET_CAR) {
					s.carMode = CAR_MODE_CHASE_BEHIND;
					s.carModeIndex = 0;
					s.carModeStart = now;
				}
			}
			s.camSnapped = 0;
		}

		if (!s.leadMode)
		{
			int spoolRegion = AntFarmRegionOf(&s.spool);
			int regionsReady;
			int nodesReady = (NumDriver2Straights > 0);

			/* The engine only ever pre-loads NEIGHBOURING regions as you move
			 * (CheckUnpackNewRegions / force_load_boundary — map.c:222-286), so
			 * the region a far hop lands in is NEVER put into a barrel. That is
			 * why the residency gate could not pass, every cut hit the black
			 * cap, and the shot was forced through into unloaded geometry
			 * (skybox/nodraw). Force the destination region in ourselves. */
			if (spoolRegion >= 0 && spoolRegion < regions_across * regions_down &&
				!AntFarmRegionUnpacked(spoolRegion))
			{
				int brx = spoolRegion % regions_across;
				int brz = spoolRegion / regions_across;
				int barrel = (brx & 1) + (brz & 1) * 2;

				if (loading_region[barrel] == -1)
					UnpackRegion(spoolRegion, barrel);
			}

			regionsReady = (AntFarmRegionHasData(spoolRegion) && AntFarmRegionsReady(spoolRegion));

			MainPlayer.spoolXZ = &s.spool;

			if (!(regionsReady && nodesReady) && !s.streamDone)
			{
				if (now - s.cutStart >= ANTFARM_STREAM_TIMEOUT_MS)
				{
					if (s.streamRetries < 3)
					{
						s.streamRetries++;
						s.cutStart = now;

						/* Attempt to pick a new loaded area; if fails, keep current position */
						if (AntFarmPickFarArea())
						{
							s.spool = s.areaPos;
							s.targetPos = s.areaPos;
						}
						/* else keep existing spool (which is loaded) */
						s.camSnapped = 0;
					}
					else
					{
						s.ctx->jer_log(s.ctx, "[antfarm] warning: area stream timeout — forcing progression with fallback position\n");
						/* Force use of current camera position as fallback (which is loaded) */
						s.areaPos.vx = camera_position.vx;
						s.areaPos.vz = camera_position.vz;
						s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
						AntFarmClampToWorld(&s.areaPos);
						s.spool = s.areaPos;
						s.targetPos = s.areaPos;
						s.streamDone = 1;
						s.cutStart = now;
					}
				}

				if (now - s.cutEnter >= ANTFARM_BLACK_CAP_MS)
				{
					{
						int brx = spoolRegion % regions_across;
						int brz = spoolRegion / regions_across;
						int barrel = (brx & 1) + (brz & 1) * 2;

						s.ctx->jer_log(s.ctx,
							"[antfarm] diag: area %d,%d -> region %d (rx=%d rz=%d barrel=%d unpacked=%d loading=%d data=%d) engine: current=%d rx=%d rz=%d cell=%d,%d\n",
							s.areaPos.vx, s.areaPos.vz, spoolRegion, brx, brz, barrel,
							regions_unpacked[barrel], loading_region[barrel],
							(spoolinfo_offsets[spoolRegion] == 0xffff) ? 0 : 1,
							current_region, region_x, region_z,
							current_barrel_region_xcell, current_barrel_region_zcell);
					}

					s.ctx->jer_log(s.ctx, "[antfarm] warning: black cap hit — forcing progression with fallback position\n");
					/* Use current camera position as fallback */
					s.areaPos.vx = camera_position.vx;
					s.areaPos.vz = camera_position.vz;
					s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
					AntFarmClampToWorld(&s.areaPos);
					s.spool = s.areaPos;
					s.targetPos = s.areaPos;
					s.streamDone = 1;
					s.cutStart = now;
				}

				break;
			}

			if (!s.streamDone && (regionsReady || s.streamRetries >= 3))
			{
				s.streamDone = 1;
				s.spool = s.targetPos;
				AntFarmPinPlayerCar();
				MainPlayer.spoolXZ = &s.spool;
				s.cutStart = now;
			}

			if (s.streamDone && !s.shotPlanned)
			{
				if (s.targetKind == ANTFARM_TARGET_CAR)
				{
					int car = AntFarmPickCarNear(&s.areaPos, 14000,
						(antStyleDefs[s.style].model == ANT_MODEL_ATTACH ||
							antStyleDefs[s.style].model == ANT_MODEL_TRIPODZ) ? 40 : 8);
					if (car >= 0)
					{
						s.targetCarId = car;
						AntFarmRememberCar(car);
						s.cutWaitForCar = 0;
						s.carMode = CAR_MODE_CHASE_BEHIND;
						s.carModeIndex = 0;
						s.carModeStart = now;
						AntFarmPlanShot();
						s.shotPlanned = 1;
					}
					else if (now - s.cutStart >= ANTFARM_CAR_WAIT_MS)
					{
						s.targetKind = ANTFARM_TARGET_ROAD;
						s.style = AntFarmPickStyle(0);
						AntFarmPlanShot();
						s.shotPlanned = 1;
					}
				}
				else
				{
					AntFarmPlanShot();
					s.shotPlanned = 1;
				}
			}
		}

		if (s.shotPlanned && now - s.cutStart >= (unsigned long)(ANTFARM_CUT_HOLD_MS
			+ (s.jumpDist2 > 0 ? (s.jumpDist2 / 30000 > 2000 ? 2000 : (int)(s.jumpDist2 / 30000)) : 0)))
		{
			if (!s.leadMode && s.leadEnabled &&
				s.targetKind == ANTFARM_TARGET_CAR &&
				MainPlayer.playerType == PLAYER_TYPE_CAR &&
				(AntRand() % 100) < s.leadChance)
			{
				AntFarmStartLead();
			}

			s.state = ANTFARM_STATE_FADE_IN;
			s.stateStart = now;
			s.shotStart = now;
			s.dwellMs = AntFarmComputeDwell();
			s.camSnapped = 0;	/* snap onto the new shot under the black */

			/* an occasional place-name caption, not on every shot */
			if (s.captions && !s.leadMode && AntRandChance(40))
				s.captionUntil = now + ANTFARM_CAPTION_MS;

			s.cutCount++;
			s.ctx->jer_log(s.ctx,
				"[antfarm] cut #%d -> area %d,%d region %d/%d style %d%s\n",
				s.cutCount, s.areaPos.vx, s.areaPos.vz,
				current_region / regions_across, current_region % regions_across,
				s.style,
				(s.targetKind == ANTFARM_TARGET_CAR) ? " car" : "");
		}
		break;

	case ANTFARM_STATE_FADE_IN:
		s.fade = 255 - (int)((now - s.stateStart) * 255 / ANTFARM_FADE_MS);

		if (s.fade <= 0)
		{
			s.fade = 0;
			s.state = ANTFARM_STATE_SHOW;
			s.stateStart = now;
			/* If car target, reset mode timer */
			if (s.targetKind == ANTFARM_TARGET_CAR) {
				s.carModeStart = now;
			}
		}
		break;
	}

	if (s.active)
		AntFarmPinPlayerCar();

	return JER_RESULT_CONTINUE;
}

/* Pin the player's car to the camera focus while the screensaver runs */
static void AntFarmPinPlayerCar(void)
{
	int carId;
	CAR_DATA* cp;

	if (MainPlayer.playerType != PLAYER_TYPE_CAR)
		return;

	carId = MainPlayer.playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	cp = &car_data[carId];

	{
		int maxX = units_across_halved - 2000;
		int maxZ = units_down_halved - 2000;
		int ground;

		if (s.targetPos.vx < -maxX)
			s.targetPos.vx = -maxX;

		if (s.targetPos.vx > maxX)
			s.targetPos.vx = maxX;

		if (s.targetPos.vz < -maxZ)
			s.targetPos.vz = -maxZ;

		if (s.targetPos.vz > maxZ)
			s.targetPos.vz = maxZ;

		ground = AntFarmMapHeight(s.targetPos.vx, s.targetPos.vz);

		if (s.targetPos.vy < ground - 100)
			s.targetPos.vy = ground;
	}

	cp->hd.where.t[0] = s.targetPos.vx;
	cp->hd.where.t[1] = s.targetPos.vy;
	cp->hd.where.t[2] = s.targetPos.vz;

	cp->st.n.fposition[0] = s.targetPos.vx << 4;
	cp->st.n.fposition[1] = s.targetPos.vy << 4;
	cp->st.n.fposition[2] = s.targetPos.vz << 4;

	cp->st.n.linearVelocity[0] = 0;
	cp->st.n.linearVelocity[1] = 0;
	cp->st.n.linearVelocity[2] = 0;
	cp->st.n.angularVelocity[0] = 0;
	cp->st.n.angularVelocity[1] = 0;
	cp->st.n.angularVelocity[2] = 0;

	cp->hd.speed = 0;

	{
		int g = AntFarmMapHeight(cp->hd.where.t[0], cp->hd.where.t[2]);

		if (cp->hd.where.t[1] < g)
			cp->hd.where.t[1] = g;

		cp->st.n.fposition[1] = cp->hd.where.t[1] << 4;
	}

	if (cp->controlType != CONTROL_TYPE_NONE)
	{
		s.savedPlayerCarControlType = cp->controlType;
		s.savedPlayerReservedSlot = reservedSlots[carId];
		cp->controlType = CONTROL_TYPE_NONE;
		reservedSlots[carId] = 1;
	}
}

/* render-time: take over the camera */
static int AntFarmOnCamera(void* userdata, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	VECTOR cam;
	SVECTOR ang;
	unsigned long now;

	(void)userdata;

	if (!s.active)
		return JER_RESULT_CONTINUE;

	now = AntTicks();

	MainPlayer.spoolXZ = &s.spool;

	/* During the CUT the shot's geometry is not final and the previous shot's
	 * road/car references are no longer resident (the spool has already moved),
	 * so hold the camera completely: it must not look at, or drift into,
	 * geometry that has not streamed. This is what keeps a cut black instead of
	 * flashing skybox/nodraw. */
	if (s.state == ANTFARM_STATE_CUT)
	{
		/* keep streaming the SUBJECT while the screen is black: the new region
		 * will evict the old one, which is fine because nothing is visible */
		s.spool = s.targetPos;

		*(VECTOR*)a->cameraPosition = s.camPos;
		*(SVECTOR*)a->cameraAngle = s.camAngle;
		camera_position = s.camPos;
		camera_angle = s.camAngle;
		a->override = 1;

		return JER_RESULT_CONTINUE;
	}

	AntFarmComputeCamera(&cam, &ang);

	if (s.aimPos.vx == 0 && s.aimPos.vy == 0 && s.aimPos.vz == 0)
	{
		*(VECTOR*)a->cameraPosition = s.camPos;
		*(SVECTOR*)a->cameraAngle = s.camAngle;
		a->override = 1;
		return JER_RESULT_CONTINUE;
	}

	AntFarmPinPlayerCar();

	AntFarmClampToWorld(&cam);
	AntFarmClampToWorld(&s.aimPos);
	AntFarmClampAboveGround(&s.aimPos);

	/* --- void guard -----------------------------------------------------
	 * The renderer culls from camera_position, but regions stream around
	 * MainPlayer.spoolXZ. If the camera or its aim sits in a region that has
	 * not streamed, the frame is skybox + nodraw. Hold the previous camera
	 * until it is resident; the cap means a data-less region can never
	 * freeze the view forever. */
	{
		int camReady = AntFarmRegionUnpacked(AntFarmRegionOf(&cam));
		int aimReady = AntFarmRegionUnpacked(AntFarmRegionOf(&s.aimPos));

		if ((!camReady || !aimReady) && s.holdSince != 0 && now - s.holdSince > 4000)
		{
			camReady = 1;	/* held long enough — take the shot anyway */
			aimReady = 1;
		}

		if (!camReady || !aimReady)
		{
			if (s.holdSince == 0)
			{
				s.holdSince = now;
				s.voidHolds++;
				s.ctx->jer_log(s.ctx,
					"[antfarm] void guard: region not resident, holding camera (#%d)\n",
					s.voidHolds);
			}

			*(VECTOR*)a->cameraPosition = s.camPos;
			*(SVECTOR*)a->cameraAngle = s.camAngle;
			camera_position = s.camPos;
			camera_angle = s.camAngle;
			a->override = 1;

			return JER_RESULT_CONTINUE;
		}

		s.holdSince = 0;
	}

	/* The streamer must follow the camera, because the renderer culls from
	 * camera_position: keeping the spool on the abstract "area" let the camera
	 * cross a region edge into a slot that was not resident — the skybox/nodraw
	 * void. (During the black CUT the spool instead tracks the planned subject,
	 * which is what has to stream in before the shot can be shown.) */
	s.spool = cam;

	{
		int ground = -AntFarmMapHeight(cam.vx, cam.vz);

		if (cam.vy > ground - 90)
			cam.vy = ground - 90;
	}

	s.camPos = cam;

	{
		SVECTOR tgt;

		PointAtTarget(&cam, &s.aimPos, &tgt);

		/* a barely-there horizon roll keeps the frame from feeling rigid */
		if (s.rollOn)
			tgt.vz = (short)(RSIN((AntTicks() / 400) & 4095) >> 7);

		{
			int cur, d;

			cur = s.camAngle.vx;
			d = tgt.vx - cur;

			if (d > 2048)
				d -= 4096;
			else if (d < -2048)
				d += 4096;

			s.camAngle.vx = (short)((cur + d * 18 / 100) & 0xfff);

			cur = s.camAngle.vy;
			d = tgt.vy - cur;

			if (d > 2048)
				d -= 4096;
			else if (d < -2048)
				d += 4096;

			s.camAngle.vy = (short)((cur + d * 18 / 100) & 0xfff);

			cur = s.camAngle.vz;
			d = tgt.vz - cur;

			if (d > 2048)
				d -= 4096;
			else if (d < -2048)
				d += 4096;

			s.camAngle.vz = (short)((cur + d * 18 / 100) & 0xfff);
		}

		ang = s.camAngle;
	}

	*(VECTOR*)a->cameraPosition = cam;
	*(SVECTOR*)a->cameraAngle = ang;

	camera_position = cam;
	camera_angle = ang;

	/* breathe the lens between shots instead of snapping it, and let a zoom row
	 * push in and ease back out across the shot */
	s.fovCurrent += (AntFarmFovTarget() - s.fovCurrent) / 18;
	SetGeomScreen(scr_z = s.fovCurrent);

	if (s.targetKind == ANTFARM_TARGET_CAR && s.targetCarId >= 0 &&
		antStyleDefs[s.style].model == ANT_MODEL_FOLLOW)
		CameraCar = s.targetCarId;

	a->override = 1;

	return JER_RESULT_CONTINUE;
}

/* on-foot input: explicit kill switch */
static int AntFarmOnPedInput(void* userdata, void* args)
{
	JER_ARGS_PED_INPUT* a = (JER_ARGS_PED_INPUT*)args;

	(void)userdata;

	if (s.active)
		a->pad = 0;

	return JER_RESULT_CONTINUE;
}

/* START during the screensaver: hand control back */
static int AntFarmOnPauseMenu(void* userdata, void* args)
{
	JER_ARGS_PAUSE_MENU* a = (JER_ARGS_PAUSE_MENU*)args;

	(void)userdata;

	if (a->action == JER_PAUSE_OPEN && s.active)
	{
		AntFarmSetActive(0);
		jer_config_set_bool(ANT_MOD_ID, "enabled", 0);
	}

	return JER_RESULT_CONTINUE;
}

/* display name for the level the player is in (see main.c bootLevelNames) */
static const char* AntFarmPlaceName(void)
{
	static const char* names[4] = { "Chicago", "Havana", "Las Vegas", "Rio" };

	if (GameLevel < 0 || GameLevel > 3)
		return "Driver 2";

	return names[GameLevel];
}

/* transition wash, optional letterbox, and an occasional place caption */
static int AntFarmOnDrawOverlay(void* userdata, void* args)
{
	POLY_F4* poly;
	int v;

	(void)userdata;
	(void)args;

	if (!s.active)
		return JER_RESULT_CONTINUE;

	/* --- transition wash (only mid-cut) --- */
	v = s.fade;

	if (v > 0)
	{
		if (v > 255)
			v = 255;

		poly = (POLY_F4*)current->primptr;

		setPolyF4(poly);
		setSemiTrans(poly, 1);
		setRGB0(poly, v, v, v);

#ifdef PSX
		setXYWH(poly, 0, 0, 320, 256);
#else
		setXYWH(poly, -500, 0, 1200, 256);
#endif

		addPrim(current->ot, poly);
		current->primptr += sizeof(POLY_F4);

		if (v >= 250)
		{
			poly = (POLY_F4*)current->primptr;

			setPolyF4(poly);
			setRGB0(poly, 0, 0, 0);

#ifdef PSX
			setXYWH(poly, 0, 0, 320, 256);
#else
			setXYWH(poly, -500, 0, 1200, 256);
#endif

			addPrim(current->ot, poly);
			current->primptr += sizeof(POLY_F4);
		}
	}

	/* --- soft letterbox: a barely-there cinematic frame --- */
	if (s.letterbox)
	{
		int i;

		for (i = 0; i < 2; i++)
		{
			int y = (i == 0) ? 0 : (256 - ANTFARM_LETTERBOX_H);

			poly = (POLY_F4*)current->primptr;

			setPolyF4(poly);
			setRGB0(poly, 0, 0, 0);

#ifdef PSX
			setXYWH(poly, 0, y, 320, ANTFARM_LETTERBOX_H);
#else
			setXYWH(poly, -500, y, 1200, ANTFARM_LETTERBOX_H);
#endif

			addPrim(current->ot, poly);
			current->primptr += sizeof(POLY_F4);
		}
	}

	/* --- occasional place-name caption, fading in and out --- */
	if (s.captions && s.captionUntil != 0)
	{
		unsigned long now = AntTicks();

		if (now < s.captionUntil)
		{
			unsigned long left = s.captionUntil - now;
			unsigned long shown = ANTFARM_CAPTION_MS - left;
			char line[24];
			int level;

			if (shown < 900)
				level = (int)(shown * 235 / 900);
			else if (left < 900)
				level = (int)(left * 235 / 900);
			else
				level = 235;

			if (level < 0) level = 0;
			if (level > 235) level = 235;

			snprintf(line, sizeof(line), "%s", AntFarmPlaceName());

			SetTextColour((u_char)level, (u_char)level, (u_char)level);
			PrintString(line, 12, 196);
		}
		else
		{
			s.captionUntil = 0;
		}
	}

	return JER_RESULT_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* pause menu                                                         */
/* ------------------------------------------------------------------ */

static void AntMenuLabel(void* userdata, char* out, int max)
{
	(void)userdata;

	snprintf(out, max, "Ant Farm: %s", s.active ? "ON" : "OFF");
}

static int AntMenuToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;

	AntFarmToggle();

	return JER_PAUSE_QUIT_NONE;
}

static void AntIntervalLabel(void* userdata, char* out, int max)
{
	(void)userdata;

	snprintf(out, max, "Cut interval: %ds", AntIntervalMs() / 1000);
}

static int AntIntervalAdjust(void* userdata, int direction)
{
	int secs;

	(void)userdata;

	secs = AntIntervalMs() / 1000 + direction * 5;

	if (secs < ANTFARM_MIN_INTERVAL)
		secs = ANTFARM_MIN_INTERVAL;

	if (secs > ANTFARM_MAX_INTERVAL)
		secs = ANTFARM_MAX_INTERVAL;

	s.intervalMs = secs * 1000;
	jer_config_set_int(ANT_MOD_ID, "interval", secs);

	return JER_PAUSE_QUIT_NONE;
}

static void AntModeIntervalLabel(void* userdata, char* out, int max)
{
	(void)userdata;

	snprintf(out, max, "Mode interval: %ds", AntCarModeIntervalMs() / 1000);
}

static int AntModeIntervalAdjust(void* userdata, int direction)
{
	int secs;

	(void)userdata;

	secs = AntCarModeIntervalMs() / 1000 + direction * 2;

	if (secs < 2)
		secs = 2;
	if (secs > 30)
		secs = 30;

	s.carModeIntervalMs = secs * 1000;
	jer_config_set_int(ANT_MOD_ID, "mode_interval", secs);

	return JER_PAUSE_QUIT_NONE;
}

static void AntModesPerCutLabel(void* userdata, char* out, int max)
{
	(void)userdata;

	snprintf(out, max, "Modes per cut: %d", s.carModesPerCut);
}

static int AntModesPerCutAdjust(void* userdata, int direction)
{
	int val = s.carModesPerCut + direction;

	if (val < 1)
		val = 1;
	if (val > 20)
		val = 20;

	s.carModesPerCut = val;
	jer_config_set_int(ANT_MOD_ID, "modes_per_cut", val);

	return JER_PAUSE_QUIT_NONE;
}

/* Style rows are generated from the archetype table, so a new camera style
 * shows up in the pause menu automatically (userdata carries the index). */
static void AntStyleLabelFn(void* userdata, char* out, int max)
{
	int style = (int)(size_t)userdata;

	snprintf(out, max, "%s: %s", antStyleDefs[style].label,
		s.stylesEnabled[style] ? "ON" : "OFF");
}

static int AntStyleToggleFn(void* userdata, int direction)
{
	int style = (int)(size_t)userdata;
	char key[24];

	(void)direction;

	s.stylesEnabled[style] = !s.stylesEnabled[style];

	snprintf(key, sizeof(key), "style_%s", antStyleDefs[style].key);
	jer_config_set_bool(ANT_MOD_ID, key, s.stylesEnabled[style]);

	return JER_PAUSE_QUIT_NONE;
}

static void AntLeadLabel(void* userdata, char* out, int max)
{
	(void)userdata;

	snprintf(out, max, "Rogue cars: %s", s.leadEnabled ? "ON" : "OFF");
}

static int AntLeadToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;

	s.leadEnabled = !s.leadEnabled;
	jer_config_set_bool(ANT_MOD_ID, "lead_mode", s.leadEnabled);

	return JER_PAUSE_QUIT_NONE;
}

static JER_PAUSE_MENU_ITEM antMenuItems[ANTFARM_STYLE_COUNT + 5];
static JER_PAUSE_MENU antFarmMenu;

static void AntFarmSetItem(int* n, void (*get_label)(void*, char*, int),
	int (*on_activate)(void*, int), void* userdata, int adjust)
{
	antMenuItems[*n].label = NULL;
	antMenuItems[*n].get_label = get_label;
	antMenuItems[*n].on_activate = on_activate;
	antMenuItems[*n].userdata = userdata;
	antMenuItems[*n].submenu = NULL;
	antMenuItems[*n].adjust = adjust;
	(*n)++;
}

/* Built at activation: item_count must equal the number of filled rows. */
static void AntFarmBuildMenu(void)
{
	int n = 0, i;

	AntFarmSetItem(&n, AntMenuLabel, AntMenuToggle, NULL, 0);
	AntFarmSetItem(&n, AntIntervalLabel, AntIntervalAdjust, NULL, 1);
	AntFarmSetItem(&n, AntModeIntervalLabel, AntModeIntervalAdjust, NULL, 1);
	AntFarmSetItem(&n, AntModesPerCutLabel, AntModesPerCutAdjust, NULL, 1);

	for (i = 0; i < ANTFARM_STYLE_COUNT; i++)
		AntFarmSetItem(&n, AntStyleLabelFn, AntStyleToggleFn, (void*)(size_t)i, 0);

	AntFarmSetItem(&n, AntLeadLabel, AntLeadToggle, NULL, 0);

	antFarmMenu.title = "Ant Farm";
	antFarmMenu.items = antMenuItems;
	antFarmMenu.item_count = n;
}

/* ------------------------------------------------------------------ */
/* boot + entry                                                       */
/* ------------------------------------------------------------------ */

static int AntFarmOnBoot(void* userdata, void* args)
{
	int secs, modeSecs, modesPerCut, i;

	(void)userdata;
	(void)args;

	secs = jer_config_get_int(ANT_MOD_ID, "interval", ANTFARM_DEFAULT_INTERVAL);

	if (secs < ANTFARM_MIN_INTERVAL)
		secs = ANTFARM_MIN_INTERVAL;

	if (secs > ANTFARM_MAX_INTERVAL)
		secs = ANTFARM_MAX_INTERVAL;

	s.intervalMs = secs * 1000;

	modeSecs = jer_config_get_int(ANT_MOD_ID, "mode_interval", 6);
	if (modeSecs < 2) modeSecs = 2;
	if (modeSecs > 30) modeSecs = 30;
	s.carModeIntervalMs = modeSecs * 1000;

	modesPerCut = jer_config_get_int(ANT_MOD_ID, "modes_per_cut", 5);
	if (modesPerCut < 1) modesPerCut = 1;
	if (modesPerCut > 20) modesPerCut = 20;
	s.carModesPerCut = modesPerCut;

	for (i = 0; i < ANTFARM_STYLE_COUNT; i++)
	{
		char key[24];
		int def;

		snprintf(key, sizeof(key), "style_%s", antStyleDefs[i].key);

		/* chase is the least restful archetype, so it is off unless asked for;
		 * an archetype whose camera has not landed yet is off too */
		def = AntFarmStyleImplemented(antStyleDefs[i].model) &&
			(i != ANTFARM_STYLE_CHASE);

		s.stylesEnabled[i] = jer_config_get_bool(ANT_MOD_ID, key, def);
	}

	s.rollOn = jer_config_get_bool(ANT_MOD_ID, "roll", 1);
	s.letterbox = jer_config_get_bool(ANT_MOD_ID, "letterbox", 1);
	s.captions = jer_config_get_bool(ANT_MOD_ID, "captions", 1);
	s.leadEnabled = jer_config_get_bool(ANT_MOD_ID, "lead_mode", 0);
	s.leadChance = ANTFARM_LEAD_CHANCE;
	s.pendingEnable = jer_config_get_bool(ANT_MOD_ID, "enabled", 0);

	/* Build the road cache once at boot */
	AntFarmBuildRoadCache();

	{
		char styleList[80];
		int n = 0;

		styleList[0] = '\0';

		for (i = 0; i < ANTFARM_STYLE_COUNT && n < (int)sizeof(styleList) - 16; i++)
		{
			if (!s.stylesEnabled[i])
				continue;

			n += snprintf(styleList + n, sizeof(styleList) - (size_t)n, "%s%s",
				(n > 0) ? "," : "", antStyleDefs[i].key);
		}

		s.ctx->jer_log(s.ctx,
			"[antfarm] ready: interval %ds, mode interval %ds, modes/cut %d, styles %s, lead=%d\n",
			s.intervalMs / 1000, s.carModeIntervalMs / 1000,
			s.carModesPerCut, styleList, s.leadEnabled);
	}

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_antfarm_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&s, 0, sizeof(s));
	s.ctx = ctx;
	s.intervalMs = ANTFARM_DEFAULT_INTERVAL * 1000;
	s.carModeIntervalMs = 6000;
	s.carModesPerCut = 5;

	ctx->jer_register_module(ctx,
		"antfarm",
		"Ant Farm Screensaver",
		"0.1.0",
		"REDRIVER2 community",
		"City-observer screensaver with dynamic car-mode cycling, diverse cinematic camera styles touring the whole map, with optional rogue-car chases.",
		"",
		JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, AntFarmOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, AntFarmOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, AntFarmOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, AntFarmOnCamera, NULL, 100);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, AntFarmOnPedInput, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, AntFarmOnPauseMenu, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, AntFarmOnDrawOverlay, NULL, 0);

	AntFarmBuildMenu();
	jer_pause_menu_register(&antFarmMenu);

	ctx->jer_log(ctx, "[antfarm] registered (SDK v%d)\n", ctx->sdkVersion);
}