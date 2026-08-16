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

/* style pick weighting — static/overhead dominate the chase cam */
static const int antStyleWeights[ANTFARM_STYLE_COUNT] = { 10, 30, 30, 15, 15 };

/* ------------------------------------------------------------------ */
/* cached usable road list (fast random & nearest)                    */
/* ------------------------------------------------------------------ */
static int* g_usableRoads = NULL;
static int g_numUsableRoads = 0;
static int g_usableRoadsAlloc = 0;

/* spool.c / map.c — unpacked status of each region (1 = loaded) */
extern int regions_unpacked[];
/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

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

	int intervalMs;					/* seconds per visible shot */
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

	/* per-shot framing */
	int shotSideSign;	/* +1/-1 — which side of the road the camera sits */
	int shotMargin;		/* clearance from the road half-width edge */
	int shotHeight;		/* camera elevation above the road/ground */
	int shotLookAhead;	/* how far ahead of the camera the aim sits */
	int shotOrbitAmp;	/* tripod azimuth sweep amplitude (0 = static) */
	int shotOrbitPhase;	/* orbit phase seed */
	int shotScrZ;		/* per-shot FOV (projection distance) */
	int armFrac;		/* smoothed LOS pull-back fraction (256 = full arm) */

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
static void AntFarmPickStyleAndTarget(void);
static void AntFarmBuildRoadCache(void);

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
	if (n <= 0) {
		g_numUsableRoads = 0;
		return;
	}
	if (g_usableRoadsAlloc < n) {
		if (g_usableRoads) free(g_usableRoads);
		g_usableRoads = (int*)malloc(n * sizeof(int));
		g_usableRoadsAlloc = n;
	}
	int count = 0;
	for (int i = 0; i < n; i++) {
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[i];
		if (rd->length > 400 && rd->angle < 2048)
			g_usableRoads[count++] = i;
	}
	g_numUsableRoads = count;
}

/* pick a straight at least minLen long, using the cache */
static int AntFarmPickSurface(int minLen)
{
	if (g_numUsableRoads == 0)
		return -1;

	/* Try up to 24 random picks from the cache */
	for (int i = 0; i < 24; i++) {
		int idx = g_usableRoads[Random2(0) % g_numUsableRoads];
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

	*x = 0;
	*z = 0;

	GetNodePos(straight, NULL, curve, distAlong, NULL, x, z, laneNo);

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

/* Fast check: central region + 4 cardinal neighbours must be unpacked.
 * This is much quicker than the full 3×3 check, letting us fade in as
 * soon as roads and basic geometry are available. */
static int AntFarmRegionsReady(int centerRegion)
{
	int rx = centerRegion / regions_across;
	int rz = centerRegion % regions_across;
	int offsets[5][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1} };

	for (int i = 0; i < 5; i++) {
		int nrx = rx + offsets[i][0];
		int nrz = rz + offsets[i][1];
		if (nrx < 0 || nrx >= regions_across || nrz < 0 || nrz >= regions_down)
			return 0;
		int idx = nrx + nrz * regions_across;
		if (spoolinfo_offsets[idx] == 0xffff)  // no geometry at all
			return 0;
		if (!regions_unpacked[idx])            // not yet loaded
			return 0;
	}
	return 1;
}

/* does this region have spool data at all? (0xffff = none — approaching it
 * would show a void forever) */
static int AntFarmRegionHasData(int region)
{
	if (region < 0 || region >= regions_across * regions_down)
		return 0;

	return spoolinfo_offsets[region] != 0xffff;
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

/* pick a road – preferably in a different region, but never give up */
static int AntFarmPickFarArea(void)
{
	if (g_numUsableRoads == 0) {
		/* No usable roads at all – fallback to camera position */
		s.areaPos.vx = camera_position.vx;
		s.areaPos.vz = camera_position.vz;
		s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
		AntFarmClampToWorld(&s.areaPos);
		return 1;
	}

	int curRegion = AntFarmRegionOf(&s.targetPos);

	/* Try up to 50 times to find a road in a different region */
	for (int attempt = 0; attempt < 50; attempt++) {
		int idx = g_usableRoads[Random2(0) % g_numUsableRoads];
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
		if (rd->length < 200) continue;   // skip very short ones

		VECTOR pos;
		pos.vx = rd->Midx;
		pos.vy = AntFarmMapHeight(rd->Midx, rd->Midz);
		pos.vz = rd->Midz;
		AntFarmClampToWorld(&pos);

		if (AntFarmRegionOf(&pos) != curRegion) {
			s.areaPos = pos;
			return 1;
		}
	}

	/* Fallback: pick any usable road (even in same region) – never fail */
	for (int attempt = 0; attempt < 30; attempt++) {
		int idx = g_usableRoads[Random2(0) % g_numUsableRoads];
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[idx];
		if (rd->length < 200) continue;
		s.areaPos.vx = rd->Midx;
		s.areaPos.vy = AntFarmMapHeight(rd->Midx, rd->Midz);
		s.areaPos.vz = rd->Midz;
		AntFarmClampToWorld(&s.areaPos);
		return 1;
	}

	/* Ultimate fallback: use current camera position (clamped) */
	s.areaPos.vx = camera_position.vx;
	s.areaPos.vz = camera_position.vz;
	s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
	AntFarmClampToWorld(&s.areaPos);
	return 1;
}

/* pick a moving civilian car near a point (world units), or -1 */
static int AntFarmPickCarNear(const VECTOR* area, int radius)
{
	int i, n = 0, pick = -1;
	long long r2 = (long long)radius * radius;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];
		long long dx, dz;

		if (cp->controlType != CONTROL_TYPE_CIV_AI)
			continue;

		if (ABS(cp->hd.speed) <= 8)
			continue;

		dx = (long long)cp->hd.where.t[0] - area->vx;
		dz = (long long)cp->hd.where.t[2] - area->vz;

		if (dx * dx + dz * dz > r2)
			continue;

		if (Random2(0) % (n + 1) == 0)
			pick = i;

		n++;
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

/* weighted style pick. carOnly: 1 = chase/static/overhead, 0 = overhead/
 * tripod/flyover, -1 = any. Overhead fits both. */
static int AntFarmPickStyle(int carOnly)
{
	int total = 0, i, roll;

	for (i = 0; i < ANTFARM_STYLE_COUNT; i++)
	{
		int isCar = (i == ANTFARM_STYLE_CHASE || i == ANTFARM_STYLE_STATIC);
		int isRoad = (i == ANTFARM_STYLE_TRIPOD || i == ANTFARM_STYLE_FLYOVER);

		if (!s.stylesEnabled[i])
			continue;

		if (carOnly == 1 && isRoad)
			continue;

		if (carOnly == 0 && isCar)
			continue;

		total += antStyleWeights[i];
	}

	if (total == 0)
		return ANTFARM_STYLE_TRIPOD;

	roll = Random2(0) % total;

	for (i = 0; i < ANTFARM_STYLE_COUNT; i++)
	{
		int isCar = (i == ANTFARM_STYLE_CHASE || i == ANTFARM_STYLE_STATIC);
		int isRoad = (i == ANTFARM_STYLE_TRIPOD || i == ANTFARM_STYLE_FLYOVER);

		if (!s.stylesEnabled[i])
			continue;

		if (carOnly == 1 && isRoad)
			continue;

		if (carOnly == 0 && isCar)
			continue;

		roll -= antStyleWeights[i];

		if (roll < 0)
			return i;
	}

	return ANTFARM_STYLE_TRIPOD;
}

/* randomize the framing of a freshly picked shot so no two cuts look alike */
static void AntFarmInitShotVars(void)
{
	s.shotSideSign = (Random2(0) & 1) ? 1 : -1;
	s.shotMargin = 240 + (Random2(0) % 160);	/* 240..400 clear of the kerb */
	s.shotLookAhead = 560 + (Random2(0) % 640);	/* 560..1200 */
	s.shotOrbitAmp = 0;
	s.armFrac = 256;
	s.shotOrbitPhase = AntTicks() & 4095;

	switch (s.style)
	{
	case ANTFARM_STYLE_CHASE:
		s.shotHeight = 240 + (Random2(0) % 120);
		s.shotScrZ = 240 + (Random2(0) % 20);
		break;

	case ANTFARM_STYLE_STATIC:
		s.shotHeight = 220 + (Random2(0) % 90);
		s.shotScrZ = 244 + (Random2(0) % 20);
		break;

	case ANTFARM_STYLE_OVERHEAD:
		s.shotHeight = 360 + (Random2(0) % 260);	/* scenic high view */
		s.shotScrZ = 36 + (Random2(0) % 24);
		break;

	case ANTFARM_STYLE_FLYOVER:
		s.shotHeight = 500 + (Random2(0) % 250);
		s.shotScrZ = 218 + (Random2(0) % 24);
		break;

	default:	/* ANTFARM_STYLE_TRIPOD */
		s.shotHeight = 240 + (Random2(0) % 140);

		if (Random2(0) % 3 == 0)
			s.shotOrbitAmp = 284 + (Random2(0) % 384);	/* ±34°..±67° */

		s.shotScrZ = 80 + (Random2(0) % 24);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* shot planning (called during the black CUT)                        */
/* ------------------------------------------------------------------ */

/* point the shot's road reference at s.roadSurfId (the area road): pick a
 * lane + distance and set the spool anchor. Sampling near a straight's END
 * frames junctions (junctions have no centre of their own). */
static void AntFarmSetupRoadShot(void)
{
	DRIVER2_STRAIGHT* rd;
	int nLanes;
	int roll;
	int len;

	if (s.roadSurfId < 0 || s.roadSurfId >= NumDriver2Straights)
	{
		s.targetPos = s.areaPos;
		return;
	}

	rd = &Driver2StraightsPtr[s.roadSurfId];
	len = rd->length;

	nLanes = ROAD_WIDTH_IN_LANES(rd);
	s.roadLane = (nLanes > 0) ? (Random2(0) % nLanes) : 0;

	if (s.style == ANTFARM_STYLE_TRIPOD)
	{
		int endShot;

		roll = Random2(0) % 100;
		endShot = (roll < 40) ? 1 : (roll < 70) ? -1 : 0;	/* 1 far end, -1 near end */

		if (endShot != 0)
		{
			s.roadDist = (endShot > 0) ? (len - 300 - (Random2(0) % 500))
				: (300 + (Random2(0) % 500));

			if (Random2(0) % 100 < 45)
			{
				s.roadDist = (endShot > 0) ? (len + 260 + (Random2(0) % 320))
					: (-260 - (Random2(0) % 320));
				s.junctionCorner = 1;
			}
			else
			{
				s.junctionCorner = 0;
			}
		}
		else
		{
			s.roadDist = len / 2 + (Random2(0) % (len / 4)) - len / 8;
			s.junctionCorner = 0;
		}
	}
	else if (s.style == ANTFARM_STYLE_FLYOVER)
	{
		/* Start the dolly at a negative offset so it begins before the road start,
		 * and aim further ahead to avoid the camera pointing straight down at the end. */
		int offset = 300 + (Random2(0) % 200);
		s.roadDist = -offset;   /* start before the road */
		s.junctionCorner = 0;
		s.shotLookAhead = 800 + (Random2(0) % 400);
	}
	else	/* OVERHEAD (road) */
	{
		s.roadDist = len / 2 + (Random2(0) % (len / 4)) - len / 8;
		s.junctionCorner = 0;
	}

	/* corner shots may sit just past either end of the road */
	if (s.roadDist < -200)
		s.roadDist = -200;

	if (s.roadDist > len + 200)
		s.roadDist = len + 200;

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
	if (s.style == ANTFARM_STYLE_CHASE || s.style == ANTFARM_STYLE_STATIC) {
		s.targetKind = ANTFARM_TARGET_CAR;
	}
	else if (s.style == ANTFARM_STYLE_OVERHEAD) {
		s.targetKind = (Random2(0) & 1) ? ANTFARM_TARGET_CAR : ANTFARM_TARGET_ROAD;
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

	if (g_numUsableRoads == 0 || g_usableRoadsAlloc < NumDriver2Straights)
		AntFarmBuildRoadCache();

	if (s.targetKind == ANTFARM_TARGET_ROAD) {
		int idx = AntFarmPickRoadNear(s.areaPos.vx, s.areaPos.vz);
		if (idx >= 0) {
			if (s.style == ANTFARM_STYLE_FLYOVER && Driver2StraightsPtr[idx].length < 1600) {
				int better = AntFarmPickSurface(1600);
				if (better >= 0) idx = better;
			}
			s.roadSurfId = idx;
			s.shotScrZ = 240;
			AntFarmSetupRoadShot();
		}
		else {
			/* No road found – fallback to a safe hover above areaPos */
			s.roadSurfId = -1;
			s.targetPos = s.areaPos;
			/* Set a default FOV so the camera doesn't go crazy */
			s.shotScrZ = 240;
		}
	}
	else {
		s.targetPos = s.areaPos;
	}
	AntFarmInitShotVars();
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
		int side = (Random2(0) & 1) ? 1024 : 3072;
		int spotDist = 1000 + (Random2(0) % 400);
		int sideDist = 650 + (Random2(0) % 450);
		int hgt = 230 + (Random2(0) % 90);

		s.trackCamPos.vx = carX + FIXEDH(RSIN((dir + 2048) & 0xfff) * spotDist);
		s.trackCamPos.vz = carZ + FIXEDH(RCOS((dir + 2048) & 0xfff) * spotDist);
		s.trackCamPos.vy = -(AntFarmMapHeight(s.trackCamPos.vx, s.trackCamPos.vz) + hgt);

		s.trackCamPos.vx += FIXEDH(RSIN((dir + side) & 0xfff) * sideDist);
		s.trackCamPos.vz += FIXEDH(RCOS((dir + side) & 0xfff) * sideDist);

		s.trackPlaced = 1;
	}

	*desired = s.trackCamPos;
}

static void AntFarmComputeCamera(VECTOR* outPos, SVECTOR* outAngle)
{
	VECTOR desired = { 0, 0, 0 };
	VECTOR aim = { 0, 0, 0 };
	unsigned long now = AntTicks();
	int lerp;

	if (s.targetKind == ANTFARM_TARGET_CAR)
	{
		CAR_DATA* cp = AntFarmValidCar();
		VECTOR carPos;
		int dir, dist, height, h;

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

		switch (s.style)
		{
		case ANTFARM_STYLE_CHASE:
			AntFarmCarFraming(cp, &dist, &height);

			desired.vx = carPos.vx + FIXEDH(RSIN((dir + 2048) & 0xfff) * dist);
			desired.vy = -h - height;
			desired.vz = carPos.vz + FIXEDH(RCOS((dir + 2048) & 0xfff) * dist);
			lerp = 8;
			break;

		case ANTFARM_STYLE_OVERHEAD:
			dist = 520 + AntFarmCarSize(cp) / 2;
			height = 540 + AntFarmCarSize(cp) / 2;

			if (dist > 950)
				dist = 250;

			if (height > 820)
				height = 820;

			desired.vx = carPos.vx + FIXEDH(RSIN((dir + 2048) & 0xfff) * dist);
			desired.vy = -h - height;
			desired.vz = carPos.vz + FIXEDH(RCOS((dir + 2048) & 0xfff) * dist);
			lerp = 9;
			break;

		default:	/* ANTFARM_STYLE_STATIC */
			AntFarmStaticTrack(cp, &desired);
			lerp = 8;
			break;
		}

		aim = carPos;
		aim.vy = -(carPos.vy + 50);

		s.spool = carPos;
		s.targetPos = carPos;

		lerp = (s.style == ANTFARM_STYLE_STATIC) ? 4 : lerp;
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
			lerp = 12;
		}
		else {
			if (AntFarmShotRoadRender(s.roadDist, &roadPt, &heading) == 0)
			{
				*outPos = s.camPos;
				*outAngle = s.camAngle;
				return;
			}

			if (s.style == ANTFARM_STYLE_OVERHEAD || s.style == ANTFARM_STYLE_TRIPOD)
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

				if (s.shotOrbitAmp > 0 && s.style == ANTFARM_STYLE_TRIPOD)
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
			else	/* ANTFARM_STYLE_FLYOVER */
			{
				VECTOR startPt = { 0, 0, 0 };
				VECTOR endPt = { 0, 0, 0 };
				VECTOR aimPt = { 0, 0, 0 };
				int len = Driver2StraightsPtr[s.roadSurfId].length;
				int dummyHeading = 0;
				int t, tt, curDist, aimDist;
				unsigned long shotElapsed = now - s.shotStart;

				int startD = s.roadDist;
				int endD = len + 300 + (Random2(0) % 200);

				t = (shotElapsed >= (unsigned long)AntIntervalMs()) ? 1000
					: (int)(shotElapsed * 1000 / (unsigned long)AntIntervalMs());

				tt = t * t * (1000 - 2 * t) / 1000000;

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

		lerp = 12;
	}

	s.aimPos = aim;

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

		gStopPadReads = 1;
		gDoOverlays = 0;
		CopsAllowed = 0;
		// SetMasterVolume not called – traffic sounds remain

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
			s.targetCarId = AntFarmPickCarNear(&s.areaPos, 12000);

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

		s.ctx->jer_log(s.ctx,
			"[antfarm] enabled (interval %ds, styles %d%d%d%d%d, lead=%d%%)\n",
			AntIntervalMs() / 1000,
			s.stylesEnabled[ANTFARM_STYLE_CHASE],
			s.stylesEnabled[ANTFARM_STYLE_STATIC],
			s.stylesEnabled[ANTFARM_STYLE_OVERHEAD],
			s.stylesEnabled[ANTFARM_STYLE_TRIPOD],
			s.stylesEnabled[ANTFARM_STYLE_FLYOVER],
			s.leadEnabled ? s.leadChance : 0);
	}
	else
	{
		if (s.leadMode)
			AntFarmEndLead();

		gStopPadReads = s.savedStopPadReads;
		gDoOverlays = s.savedDoOverlays;
		CopsAllowed = s.savedCopsAllowed;
		SetMasterVolume(s.savedMasterVolume);

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
			"[antfarm] disabled (cuts: %d; restored pads=%d overlays=%d cops=%d vol=%d)\n",
			s.cutCount, s.savedStopPadReads, s.savedDoOverlays,
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

	if (!s.active)
		return JER_RESULT_CONTINUE;

	if (gInGameCutsceneActive || quick_replay || game_over)
	{
		AntFarmSetActive(0);
		return JER_RESULT_CONTINUE;
	}

	now = AntTicks();

	if (s.pendingEnable && !s.active &&
		!game_over && !gInGameCutsceneActive && !quick_replay &&
		NumPlayers == 1 && !NoPlayerControl)
	{
		AntFarmSetActive(1);

		if (s.active)
			s.pendingEnable = 0;
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

		if (!s.leadEnding && now - s.stateStart >= (unsigned long)AntIntervalMs())
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

			if (s.leadMode)
			{
				s.style = AntFarmPickStyle(1);
				s.trackPlaced = 0;
				AntFarmInitShotVars();
				s.shotPlanned = 1;
			}
			else
			{
				if (!AntFarmPickFarArea())
				{
					s.areaPos = s.targetPos;
				}
				s.spool = s.areaPos;
				s.targetPos = s.areaPos;
				AntFarmPickStyleAndTarget();
				s.cutWaitForCar = (s.targetKind == ANTFARM_TARGET_CAR);
			}
			s.camSnapped = 0;
		}

		if (!s.leadMode)
		{
			int spoolRegion = AntFarmRegionOf(&s.spool);
			int regionsReady = (AntFarmRegionHasData(spoolRegion) && AntFarmRegionsReady(spoolRegion));
			int nodesReady = (NumDriver2Straights > 0);

			MainPlayer.spoolXZ = &s.spool;

			if (!(regionsReady && nodesReady) && !s.streamDone)
			{
				if (now - s.cutStart >= ANTFARM_STREAM_TIMEOUT_MS)
				{
					if (s.streamRetries < 3)
					{
						s.streamRetries++;
						s.cutStart = now;

						if (AntFarmPickFarArea())
						{
							s.spool = s.areaPos;
							s.targetPos = s.areaPos;
						}
						s.camSnapped = 0;
					}
					else
					{
						s.ctx->jer_log(s.ctx, "[antfarm] warning: area stream timeout — forcing progression\n");
						s.streamDone = 1;
						s.cutStart = now;
					}
				}

				if (now - s.cutEnter >= ANTFARM_BLACK_CAP_MS)
				{
					s.ctx->jer_log(s.ctx, "[antfarm] warning: black cap hit — forcing progression\n");
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
					int car = AntFarmPickCarNear(&s.areaPos, 14000);
					if (car >= 0)
					{
						s.targetCarId = car;
						s.cutWaitForCar = 0;
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

		if (s.shotPlanned && now - s.cutStart >= ANTFARM_CUT_HOLD_MS)
		{
			if (!s.leadMode && s.leadEnabled &&
				s.targetKind == ANTFARM_TARGET_CAR &&
				MainPlayer.playerType == PLAYER_TYPE_CAR &&
				(Random2(0) % 100) < s.leadChance)
			{
				AntFarmStartLead();
			}

			s.state = ANTFARM_STATE_FADE_IN;
			s.stateStart = now;
			s.shotStart = now;

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

/* Keep the camera clear of scenery:
 *  (1) line-of-sight: if a building blocks the camera→aim ray, pull the
 *      camera in toward the aim until the ray is clear (smoothed so the
 *      pull doesn't jitter between frames);
 *  (2) push-out: run the engine's camera collider at the final position so
 *      the camera can never sit inside a building.
 *  (3) Extra safety: after collider, clamp above ground and re-check LOS.
 */
static void AntFarmSceneryPass(VECTOR* camPos, const VECTOR* aim)
{
	VECTOR wa, wb;
	int arm = 256; /* 256 = full camera arm retained */
	int i;
	static const int fracs[4] = { 192, 128, 80, 40 };

	/* world-space copies of the sight line */
	wa = *camPos;
	wa.vy = -wa.vy;
	wb = *aim;
	wb.vy = -wb.vy;

	/* (1) LOS pull-back */
	if (lineClear(&wa, &wb) == 0) {
		arm = fracs[3]; /* worst case: close shot */
		for (i = 0; i < 4; i++) {
			VECTOR test;
			test.vx = wb.vx + (wa.vx - wb.vx) * fracs[i] / 256;
			test.vy = wb.vy + (wa.vy - wb.vy) * fracs[i] / 256;
			test.vz = wb.vz + (wa.vz - wb.vz) * fracs[i] / 256;
			if (lineClear(&test, &wb) != 0) {
				arm = fracs[i];
				break;
			}
		}
	}

	/* smooth the pull so consecutive fractions blend instead of popping */
	s.armFrac += (arm - s.armFrac) * 30 / 100;   /* was 30/10 – fixed */
	if (s.armFrac > 256) s.armFrac = 256;
	arm = s.armFrac;

	/* Apply arm to camera position */
	camPos->vx = aim->vx + (camPos->vx - aim->vx) * arm / 256;
	camPos->vy = aim->vy + (camPos->vy - aim->vy) * arm / 256;
	camPos->vz = aim->vz + (camPos->vz - aim->vz) * arm / 256;

	/* (2) camera collider push-out */
	{
		CAR_DATA* jcam = &car_data[CAMERA_COLLIDER_CARID];
		ClearMem((char*)jcam, sizeof(CAR_DATA));

		jcam->controlType = CONTROL_TYPE_CAMERACOLLIDER;
		jcam->hd.direction = s.camAngle.vy & 0xfff;

		jcam->hd.where.t[0] = camPos->vx;
		jcam->hd.where.t[1] = -camPos->vy;
		jcam->hd.where.t[2] = camPos->vz;

		jcam->hd.oBox.location.vx = camPos->vx;
		jcam->hd.oBox.location.vy = -camPos->vy;
		jcam->hd.oBox.location.vz = camPos->vz;

		CheckScenaryCollisions(jcam);

		camPos->vx = jcam->hd.where.t[0];
		camPos->vy = -jcam->hd.where.t[1];
		camPos->vz = jcam->hd.where.t[2];
	}

	/* (3) Clamp above ground after collider */
	AntFarmClampAboveGround(camPos);

	/* (4) Re-check LOS; if still blocked, try alternative positions */
	{
		VECTOR test = *camPos;
		test.vy = -test.vy;
		if (lineClear(&test, &wb) == 0) {
			/* Try multiple offsets around the aim */
			int offsets[8][2] = {
				{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
				{ 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 }
			};
			int bestIdx = -1;
			int bestDist = -1;
			int curArm = arm;   /* current arm fraction (0-256) */
			int radius = (curArm * 300) / 256;   /* try positions within this radius */

			for (i = 0; i < 8; i++) {
				VECTOR cand;
				cand.vx = aim->vx + (offsets[i][0] * radius) / 4;
				cand.vz = aim->vz + (offsets[i][1] * radius) / 4;
				cand.vy = aim->vy;   /* start at aim height */
				AntFarmClampAboveGround(&cand);

				/* Re-apply collider to this candidate? For speed we just check LOS */
				VECTOR candW = cand;
				candW.vy = -candW.vy;
				if (lineClear(&candW, &wb) != 0) {
					/* This candidate has clear LOS – choose it */
					*camPos = cand;
					/* Also re-run the collider to ensure it's not inside geometry */
					{
						CAR_DATA* jcam2 = &car_data[CAMERA_COLLIDER_CARID];
						ClearMem((char*)jcam2, sizeof(CAR_DATA));
						jcam2->controlType = CONTROL_TYPE_CAMERACOLLIDER;
						jcam2->hd.direction = s.camAngle.vy & 0xfff;
						jcam2->hd.where.t[0] = cand.vx;
						jcam2->hd.where.t[1] = -cand.vy;
						jcam2->hd.where.t[2] = cand.vz;
						jcam2->hd.oBox.location.vx = cand.vx;
						jcam2->hd.oBox.location.vy = -cand.vy;
						jcam2->hd.oBox.location.vz = cand.vz;
						CheckScenaryCollisions(jcam2);
						camPos->vx = jcam2->hd.where.t[0];
						camPos->vy = -jcam2->hd.where.t[1];
						camPos->vz = jcam2->hd.where.t[2];
					}
					AntFarmClampAboveGround(camPos);
					return;   /* success */
				}
			}

			/* If no side offset works, fallback to a position directly above the aim */
			{
				int ground = -AntFarmMapHeight(aim->vx, aim->vz);
				camPos->vx = aim->vx;
				camPos->vz = aim->vz;
				camPos->vy = ground - 120;   /* 120 units above ground */
				/* Run collider one last time */
				CAR_DATA* jcam3 = &car_data[CAMERA_COLLIDER_CARID];
				ClearMem((char*)jcam3, sizeof(CAR_DATA));
				jcam3->controlType = CONTROL_TYPE_CAMERACOLLIDER;
				jcam3->hd.direction = s.camAngle.vy & 0xfff;
				jcam3->hd.where.t[0] = camPos->vx;
				jcam3->hd.where.t[1] = -camPos->vy;
				jcam3->hd.where.t[2] = camPos->vz;
				jcam3->hd.oBox.location.vx = camPos->vx;
				jcam3->hd.oBox.location.vy = -camPos->vy;
				jcam3->hd.oBox.location.vz = camPos->vz;
				CheckScenaryCollisions(jcam3);
				camPos->vx = jcam3->hd.where.t[0];
				camPos->vy = -jcam3->hd.where.t[1];
				camPos->vz = jcam3->hd.where.t[2];
				AntFarmClampAboveGround(camPos);
			}
		}
	}
}

/* render-time: take over the camera */
static int AntFarmOnCamera(void* userdata, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	VECTOR cam;
	SVECTOR ang;

	(void)userdata;

	if (!s.active)
		return JER_RESULT_CONTINUE;

	MainPlayer.spoolXZ = &s.spool;

	AntFarmComputeCamera(&cam, &ang);

	if (s.aimPos.vx == 0 && s.aimPos.vy == 0 && s.aimPos.vz == 0)
	{
		*(VECTOR*)a->cameraPosition = s.camPos;
		*(SVECTOR*)a->cameraAngle = s.camAngle;
		a->override = 1;
		return JER_RESULT_CONTINUE;
	}

	AntFarmPinPlayerCar();

	AntFarmSceneryPass(&cam, &s.aimPos);

	if (s.targetKind == ANTFARM_TARGET_ROAD)
	{
		int groundY = AntFarmMapHeight(cam.vx, cam.vz);
		if (groundY == 0 && (cam.vy < -10 || cam.vy > 10))
			groundY = -cam.vy;

		s.spool.vx = cam.vx;
		s.spool.vz = cam.vz;
		s.spool.vy = groundY;

		s.targetPos.vx = cam.vx;
		s.targetPos.vy = groundY;
		s.targetPos.vz = cam.vz;
	}

	AntFarmClampToWorld(&cam);
	AntFarmClampToWorld(&s.aimPos);
	AntFarmClampAboveGround(&s.aimPos);

	{
		int ground = -AntFarmMapHeight(cam.vx, cam.vz);

		if (cam.vy > ground - 90)
			cam.vy = ground - 90;
	}

	s.camPos = cam;

	{
		SVECTOR tgt;

		PointAtTarget(&cam, &s.aimPos, &tgt);

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

	SetGeomScreen(scr_z = s.shotScrZ);

	if (s.targetKind == ANTFARM_TARGET_CAR && s.targetCarId >= 0 &&
		s.style == ANTFARM_STYLE_CHASE)
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

/* fade wash */
static int AntFarmOnDrawOverlay(void* userdata, void* args)
{
	POLY_F4* poly;
	int v;

	(void)userdata;
	(void)args;

	if (!s.active || s.fade <= 0)
		return JER_RESULT_CONTINUE;

	v = s.fade;

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

static void AntStyleLabel(int style, const char* name, char* out, int max)
{
	snprintf(out, max, "%s: %s", name, s.stylesEnabled[style] ? "ON" : "OFF");
}

static int AntStyleToggle(int style, const char* key)
{
	s.stylesEnabled[style] = !s.stylesEnabled[style];
	jer_config_set_bool(ANT_MOD_ID, key, s.stylesEnabled[style]);

	return JER_PAUSE_QUIT_NONE;
}

static void AntChaseLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntStyleLabel(ANTFARM_STYLE_CHASE, "Chase cam", out, max);
}

static int AntChaseToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntStyleToggle(ANTFARM_STYLE_CHASE, "style_chase");
}

static void AntStaticLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntStyleLabel(ANTFARM_STYLE_STATIC, "Static track", out, max);
}

static int AntStaticToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntStyleToggle(ANTFARM_STYLE_STATIC, "style_static");
}

static void AntOverheadLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntStyleLabel(ANTFARM_STYLE_OVERHEAD, "Overhead", out, max);
}

static int AntOverheadToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntStyleToggle(ANTFARM_STYLE_OVERHEAD, "style_overhead");
}

static void AntTripodLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntStyleLabel(ANTFARM_STYLE_TRIPOD, "Tripod", out, max);
}

static int AntTripodToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntStyleToggle(ANTFARM_STYLE_TRIPOD, "style_tripod");
}

static void AntFlyoverLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntStyleLabel(ANTFARM_STYLE_FLYOVER, "Flyover", out, max);
}

static int AntFlyoverToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntStyleToggle(ANTFARM_STYLE_FLYOVER, "style_flyover");
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

static const JER_PAUSE_MENU_ITEM antMenuItems[] = {
	{ NULL, AntMenuLabel, AntMenuToggle, NULL, NULL, 0 },
	{ NULL, AntIntervalLabel, AntIntervalAdjust, NULL, NULL, 1 },
	{ NULL, AntChaseLabel,    AntChaseToggle,    NULL, NULL, 0 },
	{ NULL, AntStaticLabel,   AntStaticToggle,   NULL, NULL, 0 },
	{ NULL, AntOverheadLabel, AntOverheadToggle, NULL, NULL, 0 },
	{ NULL, AntTripodLabel,   AntTripodToggle,   NULL, NULL, 0 },
	{ NULL, AntFlyoverLabel,  AntFlyoverToggle,  NULL, NULL, 0 },
	{ NULL, AntLeadLabel,     AntLeadToggle,     NULL, NULL, 0 },
};

static const JER_PAUSE_MENU antFarmMenu = { "Ant Farm", antMenuItems, 8 };

/* ------------------------------------------------------------------ */
/* boot + entry                                                       */
/* ------------------------------------------------------------------ */

static int AntFarmOnBoot(void* userdata, void* args)
{
	int secs;

	(void)userdata;
	(void)args;

	secs = jer_config_get_int(ANT_MOD_ID, "interval", ANTFARM_DEFAULT_INTERVAL);

	if (secs < ANTFARM_MIN_INTERVAL)
		secs = ANTFARM_MIN_INTERVAL;

	if (secs > ANTFARM_MAX_INTERVAL)
		secs = ANTFARM_MAX_INTERVAL;

	s.intervalMs = secs * 1000;
	s.stylesEnabled[ANTFARM_STYLE_CHASE] = jer_config_get_bool(ANT_MOD_ID, "style_chase", 1);
	s.stylesEnabled[ANTFARM_STYLE_STATIC] = jer_config_get_bool(ANT_MOD_ID, "style_static", 1);
	s.stylesEnabled[ANTFARM_STYLE_OVERHEAD] = jer_config_get_bool(ANT_MOD_ID, "style_overhead", 1);
	s.stylesEnabled[ANTFARM_STYLE_TRIPOD] = jer_config_get_bool(ANT_MOD_ID, "style_tripod", 1);
	s.stylesEnabled[ANTFARM_STYLE_FLYOVER] = jer_config_get_bool(ANT_MOD_ID, "style_flyover", 1);
	s.leadEnabled = jer_config_get_bool(ANT_MOD_ID, "lead_mode", 0);
	s.leadChance = ANTFARM_LEAD_CHANCE;
	s.pendingEnable = jer_config_get_bool(ANT_MOD_ID, "enabled", 0);

	/* Build the road cache once at boot */
	AntFarmBuildRoadCache();

	s.ctx->jer_log(s.ctx,
		"[antfarm] ready: interval %ds, styles %d%d%d%d%d, lead=%d\n",
		s.intervalMs / 1000,
		s.stylesEnabled[ANTFARM_STYLE_CHASE],
		s.stylesEnabled[ANTFARM_STYLE_STATIC],
		s.stylesEnabled[ANTFARM_STYLE_OVERHEAD],
		s.stylesEnabled[ANTFARM_STYLE_TRIPOD],
		s.stylesEnabled[ANTFARM_STYLE_FLYOVER],
		s.leadEnabled);

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_antfarm_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&s, 0, sizeof(s));
	s.ctx = ctx;
	s.intervalMs = ANTFARM_DEFAULT_INTERVAL * 1000;

	ctx->jer_register_module(ctx,
		"antfarm",
		"Ant Farm Screensaver",
		"0.1.0",
		"REDRIVER2 community",
		"City-observer screensaver: diverse cinematic camera styles touring the whole map, with optional rogue-car chases.",
		"",
		JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, AntFarmOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, AntFarmOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, AntFarmOnCamera, NULL, 100);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, AntFarmOnPedInput, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, AntFarmOnPauseMenu, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, AntFarmOnDrawOverlay, NULL, 0);

	jer_pause_menu_register(&antFarmMenu);

	ctx->jer_log(ctx, "[antfarm] registered (SDK v%d)\n", ctx->sdkVersion);
}