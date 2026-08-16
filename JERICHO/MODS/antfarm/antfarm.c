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

#define ANT_MOD_ID "antfarm"

/* style pick weighting — static/overhead dominate the chase cam */
static const int antStyleWeights[ANTFARM_STYLE_COUNT] = { 10, 30, 30, 15, 15 };

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

/* the cut interval is config-driven; the module state can be zeroed by a
 * reload before BOOT re-runs, so never divide by the raw field */
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

	p.vx = x;
	p.vy = 0;
	p.vz = z;

	return MapHeight(&p);
}

/* pick a straight at least minLen world units long (surfId == straight
 * index); falls back to the longest road, then any road (bounded scans,
 * big cities have thousands of straights) */
static int AntFarmPickSurface(int minLen)
{
	int i;
	int n = NumDriver2Straights;
	int best = -1;
	int bestLen = 0;

	if (n <= 0)
		return -1;

	for (i = 0; i < 24; i++)
	{
		int idx = (Random2(0) + i * 17) % n;

		if (Driver2StraightsPtr[idx].length > minLen &&
		    Driver2StraightsPtr[idx].angle < 2048)
			return idx;
	}

	for (i = 0; i < n; i++)
	{
		int len = Driver2StraightsPtr[i].length;
		int usable = (len > minLen && Driver2StraightsPtr[i].angle < 2048);

		if (best < 0 && usable)
		{
			best = i;
			bestLen = len;
		}
		else if (best >= 0 && usable && len > bestLen)
		{
			best = i;
			bestLen = len;
		}
	}

	return best;	/* -1 when no camera-usable road exists (caller degrades) */
}

/* sample a point ON a road surface at a lane + distance along it, and the
 * traffic heading for that lane. x/z come out in WORLD space (positive-up);
 * the heading is 0..4095. Mirrors the civ-AI usage (GetSurfaceRoadInfo +
 * GetNodePos, civ_ai.c:2349) and the lane->heading mapping (civ_ai.c:899-922). */
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

	/* GetNodePos (civ_ai.c:989-1013) only writes *x/*z for straights with
	 * angle < 2048 when cp == NULL — guard both the write and the pick. */
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

	return rx + rz * regions_across;
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
 * starts instantly on a real road that is always inside the loaded area —
 * the camera ALWAYS moves on enable, it never keeps the player's old view
 * until the first cut. (Subsequent cuts tour far areas via the stream-gated
 * CUT.) */
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
			continue;	/* unusable for a shot (GetNodePos writes x/z) */

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
		return;	/* no usable road anywhere — keep the current view (impossible in a city) */

	s.roadSurfId = bestIdx;
	s.areaPos.vx = Driver2StraightsPtr[bestIdx].Midx;
	s.areaPos.vy = AntFarmMapHeight(s.areaPos.vx, s.areaPos.vz);
	s.areaPos.vz = Driver2StraightsPtr[bestIdx].Midz;
	AntFarmClampToWorld(&s.areaPos);
}

/* pick a straight at least FAR_DIST from the current focus, so every cut
 * tours a genuinely different part of the map */
static int AntFarmPickFarArea(void)
{
	int i;

	for (i = 0; i < 12; i++)
	{
		int idx = AntFarmPickSurface(400);
		DRIVER2_STRAIGHT* rd;

		if (idx < 0)
			break;

		rd = &Driver2StraightsPtr[idx];

		{
			long long dx = (long long)rd->Midx - s.targetPos.vx;
			long long dz = (long long)rd->Midz - s.targetPos.vz;

			if (dx * dx + dz * dz >= (long long)ANTFARM_FAR_DIST * ANTFARM_FAR_DIST)
			{
				s.roadSurfId = idx;
				s.areaPos.vx = rd->Midx;
				s.areaPos.vy = AntFarmMapHeight(rd->Midx, rd->Midz);
				s.areaPos.vz = rd->Midz;
				AntFarmClampToWorld(&s.areaPos);

				/* only tour areas the game actually has geometry for */
				if (AntFarmRegionHasData(AntFarmRegionOf(&s.areaPos)))
					return 1;
			}
		}
	}

	/* fallback: a random road wherever it is */
	s.roadSurfId = AntFarmPickSurface(400);

	if (s.roadSurfId >= 0)
	{
		DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[s.roadSurfId];

		s.areaPos.vx = rd->Midx;
		s.areaPos.vy = AntFarmMapHeight(rd->Midx, rd->Midz);
		s.areaPos.vz = rd->Midz;
		return 1;
	}

	return 0;
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
	*outHeight = 240 + vy / 2;	/* well above it */

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
		s.shotHeight = 560 + (Random2(0) % 260);	/* scenic high view */
		s.shotScrZ = 236 + (Random2(0) % 24);
		break;

	case ANTFARM_STYLE_FLYOVER:
		s.shotHeight = 500 + (Random2(0) % 250);
		s.shotScrZ = 218 + (Random2(0) % 24);
		break;

	default:	/* ANTFARM_STYLE_TRIPOD */
		s.shotHeight = 240 + (Random2(0) % 140);

		if (Random2(0) % 3 == 0)
			s.shotOrbitAmp = 384 + (Random2(0) % 384);	/* ±34°..±67° */

		s.shotScrZ = 240 + (Random2(0) % 24);
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

	if (s.roadSurfId < 0)
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

			/* 45% of junction shots sit just PAST the mouth, at the
			 * corner, looking back down the street through the junction */
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
	else
	{
		s.junctionCorner = 0;

		if (s.style == ANTFARM_STYLE_FLYOVER)
			s.roadDist = 0;	/* dolly starts at one end of the road */
		else	/* OVERHEAD (road) */
			s.roadDist = len / 2 + (Random2(0) % (len / 4)) - len / 8;
	}

	/* corner shots may sit just past either end of the road */
	if (s.roadDist < -400)
		s.roadDist = -400;

	if (s.roadDist > len + 400)
		s.roadDist = len + 400;

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

/* plan the next shot in the new far area (default touring behaviour) */
static void AntFarmPlanShot(void)
{
	s.trackPlaced = 0;
	s.targetCarId = -1;
	s.cutWaitForCar = 0;

	s.style = AntFarmPickStyle(-1);

	if (s.style == ANTFARM_STYLE_CHASE || s.style == ANTFARM_STYLE_STATIC)
	{
		s.targetKind = ANTFARM_TARGET_CAR;
		s.cutWaitForCar = 1;
	}
	else if (s.style == ANTFARM_STYLE_OVERHEAD)
	{
		if (Random2(0) & 1)
		{
			s.targetKind = ANTFARM_TARGET_CAR;
			s.cutWaitForCar = 1;
		}
		else
		{
			s.targetKind = ANTFARM_TARGET_ROAD;
			AntFarmSetupRoadShot();
		}
	}
	else	/* TRIPOD / FLYOVER */
	{
		s.targetKind = ANTFARM_TARGET_ROAD;
		AntFarmSetupRoadShot();
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
		return 1;	/* pinged out or switched */

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

	/* spawn the rogue car into a FRESH slot (the standard civ-spawn path).
	 * Re-initing a live slot with InitCar would re-run CreateDentableCar on
	 * an already-registered model and corrupt the dentable list. */
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

	/* InitCar with CONTROL_TYPE_LEAD_AI runs InitLead (hndType=5, ai.l
	 * state, currentRoad) — leadai drives it across the map (FreeRoamer) */
	InitCar(&car_data[newSlot], src->hd.direction & 0xfff, &pos,
		CONTROL_TYPE_LEAD_AI, src->ap.model, src->ap.palette & 255, NULL);

	/* retire the original civ car (it "becomes" the rogue) */
	src->controlType = CONTROL_TYPE_NONE;

	s.targetCarId = newSlot;
	s.leadMode = 1;
	s.leadEnding = 0;
	s.style = ANTFARM_STYLE_CHASE;
	s.targetKind = ANTFARM_TARGET_CAR;
	s.trackPlaced = 0;
	AntFarmInitShotVars();

	/* cops chase the (invisible) player car — which is pinned to the rogue
	 * car — so the pursuit lands on the rogue car */
	CopsAllowed = 1;
	*GetPlayerFelony(&MainPlayer) = 2500;

	s.ctx->jer_log(s.ctx, "[antfarm] rogue car went rogue — cops engaged\n");
}

static void AntFarmEndLead(void)
{
	CopsAllowed = s.savedCopsAllowed;
	*GetPlayerFelony(&MainPlayer) = (short)s.savedPlayerFelony;

	/* retire the wrecked slot so the city can reuse it (leadai has no
	 * retire path of its own; the spawner never reuses non-NONE slots) */
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

/* STATIC style: a fixed roadside camera the target car drives past. The
 * camera only rotates; it re-places itself (gently) when the car passes it
 * or drives out of reach. */
static void AntFarmStaticTrack(CAR_DATA* cp, VECTOR* desired)
{
	int carX = cp->hd.where.t[0];
	int carZ = cp->hd.where.t[2];
	int dir = cp->hd.direction;
	int dx = carX - s.trackCamPos.vx;
	int dz = carZ - s.trackCamPos.vz;
	int passed = FIXEDH(dx * RSIN(dir) + dz * RCOS(dir)) < 0;
	int far = dx * dx + dz * dz > 2200 * 2200;

	if (!s.trackPlaced || passed || far)
	{
		int side = (Random2(0) & 1) ? 1024 : 3072;
		int spotDist = 1000 + (Random2(0) % 400);	/* ahead of the car */
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
			/* target vanished this frame — hold the last position; the FRAME
			 * hook triggers an early cut */
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
			lerp = 22;
			break;

		case ANTFARM_STYLE_OVERHEAD:
			/* high behind-above follow: scenic, not chase */
			dist = 520 + AntFarmCarSize(cp) / 2;
			height = 540 + AntFarmCarSize(cp) / 2;

			if (dist > 950)
				dist = 950;

			if (height > 820)
				height = 820;

			desired.vx = carPos.vx + FIXEDH(RSIN((dir + 2048) & 0xfff) * dist);
			desired.vy = -h - height;
			desired.vz = carPos.vz + FIXEDH(RCOS((dir + 2048) & 0xfff) * dist);
			lerp = 20;
			break;

		default:	/* ANTFARM_STYLE_STATIC */
			AntFarmStaticTrack(cp, &desired);
			lerp = 8;
			break;
		}

		aim = carPos;
		aim.vy = -(carPos.vy + 50);

		/* keep the spool + player car pinned to the moving car */
		s.spool = carPos;
		s.targetPos = carPos;

		lerp = (s.style == ANTFARM_STYLE_STATIC) ? 8 : lerp;
	}
	else	/* ANTFARM_TARGET_ROAD */
	{
		VECTOR roadPt = { 0, 0, 0 };
		int heading = 0;

		if (AntFarmShotRoadRender(s.roadDist, &roadPt, &heading) == 0)
		{
			*outPos = s.camPos;
			*outAngle = s.camAngle;
			return;
		}

		if (s.style == ANTFARM_STYLE_OVERHEAD || s.style == ANTFARM_STYLE_TRIPOD)
		{
			/* PATH-SAMPLED placement: start from the sampled lane point
			 * (GetNodePos, already on the road) and offset it to exactly
			 * margin beyond the road EDGE on the chosen side — using the
			 * lane heading's perpendicular, which is the SAME convention
			 * GetNodePos uses (perp unit = (RSIN(h+1024), RCOS(h+1024))).
			 * Correct for any road width and for curves, where a fixed
			 * centreline formula would drift off the path. */
			DRIVER2_STRAIGHT* rd = &Driver2StraightsPtr[s.roadSurfId];
			int count = ROAD_LANES_COUNT(rd);
			int sideShift = count * 512 - (s.roadLane * 512 + 256);
			int dirBit = ROAD_LANE_DIR(rd, s.roadLane);
			int laneSideFromC = sideShift * ((dirBit == 0) ? 1 : -1);
			int camOffFromCentre = s.shotSideSign * (count * 512 + s.shotMargin);
			int side = (heading + (s.shotSideSign > 0 ? 1024 : 3072)) & 0xfff;
			int off = camOffFromCentre - laneSideFromC;
			int sway = RSIN((now / 7) & 4095) >> 8;
			int bobY = RSIN((now / 13) & 4095) >> 9;

			if (s.shotOrbitAmp > 0 && s.style == ANTFARM_STYLE_TRIPOD)
			{
				/* a gentle pan: ~half a sweep across the shot (~41 s/cycle
				 * at any interval — calming, never a frozen offset) */
				int sweepPhase = (s.shotOrbitPhase + now / 10) & 4095;
				int sweep = FIXEDH(RSIN(sweepPhase) * s.shotOrbitAmp);

				side = (side + 4096 + sweep) & 0xfff;
			}

			desired.vx = roadPt.vx + FIXEDH(RSIN(side) * off) + sway;
			desired.vy = roadPt.vy - s.shotHeight + bobY;
			desired.vz = roadPt.vz + FIXEDH(RCOS(side) * off);

			int aimDist = s.junctionCorner ? (s.roadDist - s.shotLookAhead)
						     : (s.roadDist + s.shotLookAhead);

			/* aim at the lane-level point, on the road surface */
			if (aimDist < 0)
				aimDist = 0;

			if (aimDist > Driver2StraightsPtr[s.roadSurfId].length)
				aimDist = Driver2StraightsPtr[s.roadSurfId].length;

			if (AntFarmShotRoadRender(aimDist, &aim, &heading) == 0)
			{
				aim = roadPt;	/* fall back to the current point */
			}

			aim.vy = aim.vy - 45;
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

			t = (shotElapsed >= (unsigned long)AntIntervalMs()) ? 1000
			    : (int)(shotElapsed * 1000 / (unsigned long)AntIntervalMs());

			/* smoothstep — eases the dolly in and out of each end */
			tt = t * t * (3000 - 2 * t) / 1000000;

			AntFarmShotRoadRender(0, &startPt, &dummyHeading);
			AntFarmShotRoadRender(len, &endPt, &dummyHeading);

			desired.vx = startPt.vx + (endPt.vx - startPt.vx) * tt / 1000;
			desired.vz = startPt.vz + (endPt.vz - startPt.vz) * tt / 1000;
			desired.vy = -(AntFarmMapHeight(desired.vx, desired.vz)) - s.shotHeight
				+ (RSIN((now / 11) & 4095) >> 6);

			curDist = (long)len * tt / 1000;
			aimDist = curDist + s.shotLookAhead;

			if (aimDist > len)
				aimDist = len;

			AntFarmShotRoadRender(aimDist, &aimPt, &dummyHeading);
			aim = aimPt;
			aim.vy = aimPt.vy - 60;
		}

		s.spool = s.targetPos;
		lerp = 14;
	}

	s.aimPos = aim;

	/* snap on a fresh target (hidden by the black), otherwise ease */
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

	PointAtTarget(&s.camPos, &aim, &s.camAngle);

	*outPos = s.camPos;
	*outAngle = s.camAngle;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void AntFarmSetActive(int on)
{
	/* an explicit disable cancels any pending auto-enable (boot said
	 * enabled, but the player just turned it off) — checked BEFORE the
	 * idempotent-return so an off-while-already-off still cancels it */
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

		/* save + cut off input, HUD, cops, sound */
		s.savedStopPadReads = gStopPadReads;
		s.savedDoOverlays = gDoOverlays;
		s.savedSpoolXZ = MainPlayer.spoolXZ;
		s.savedCopsAllowed = CopsAllowed;
		s.savedMasterVolume = gMasterVolume;
		s.savedPlayerFelony = *GetPlayerFelony(&MainPlayer);

		gStopPadReads = 1;
		gDoOverlays = 0;
		CopsAllowed = 0;	/* no felony/cop aggression while touring */
		SetMasterVolume(-10000);	/* mute cars/player SFX (music stays) */

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

		/* first shot: instantly place the camera on a nice spot in the
		 * already-loaded area and fade in fast — no stream-gated cut, so
		 * the screensaver starts on a real shot within a second, never a
		 * void (far-area touring starts from the second cut) */
		AntFarmPickNearArea();

		s.spool = s.areaPos;
		s.targetPos = s.areaPos;

		AntFarmPlanShot();

		if (s.targetKind == ANTFARM_TARGET_CAR)
		{
			s.targetCarId = AntFarmPickCarNear(&s.areaPos, 12000);

			if (s.targetCarId < 0)
			{
				/* no traffic right here — scenic road shot instead */
				s.targetKind = ANTFARM_TARGET_ROAD;
				s.style = AntFarmPickStyle(0);
				AntFarmSetupRoadShot();
				AntFarmInitShotVars();
			}
		}

		s.camSnapped = 0;	/* snap to the first shot */
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

		/* restore the spool pointer (UpdatePlayers would re-point it to the
		 * player car next frame anyway — restoring keeps the contract exact) */
		MainPlayer.spoolXZ = s.savedSpoolXZ;

		/* bring the player car back: unhide, unreserve, and put it where
		 * it was (frozen) */
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

	/* remember ON/OFF so the screensaver auto-re-enables next launch */
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

/* every world step: F9 toggle, cut state machine, rogue-car lifecycle */
static int AntFarmOnFrame(void* userdata, void* args)
{
	unsigned long now;

	(void)userdata;
	(void)args;

	AntFarmCheckF9();

	if (!s.active)
		return JER_RESULT_CONTINUE;

	/* the world changed underneath us — hand back control */
	if (gInGameCutsceneActive || quick_replay || game_over)
	{
		AntFarmSetActive(0);
		return JER_RESULT_CONTINUE;
	}

	now = AntTicks();

	/* auto-re-enable: antfarm.ini said enabled at boot — activate as soon
	 * as the game is in a playable single-player state */
	if (s.pendingEnable && !s.active &&
	    !game_over && !gInGameCutsceneActive && !quick_replay &&
	    NumPlayers == 1 && !NoPlayerControl)
	{
		AntFarmSetActive(1);

		if (s.active)
			s.pendingEnable = 0;
	}

	/* watchdog: no state may sit frozen — recover to SHOW (a screensaver
	 * must never be stuck black/white) */
	if (s.state != ANTFARM_STATE_SHOW && now - s.stateStart >= 30000)
	{
		s.ctx->jer_log(s.ctx, "[antfarm] watchdog: state %d stuck — recovering\n", s.state);
		s.fade = 0;
		s.state = ANTFARM_STATE_SHOW;
		s.stateStart = now;
	}

	/* rogue-car lifecycle runs before the state machine so it wins even
	 * mid-transition */
	if (s.leadMode)
	{
		if (AntFarmLeadEscaped())
		{
			/* left the world — wrap up now (no hold in the void) */
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

				/* cut to a fresh far area, back to touring */
				s.state = ANTFARM_STATE_FADE_OUT;
				s.stateStart = now;
			}

			return JER_RESULT_CONTINUE;
		}

		/* still chasing: keep cops hot on the rogue car */
		CopsAllowed = 1;
		*GetPlayerFelony(&MainPlayer) = 2500;
	}

	switch (s.state)
	{
	case ANTFARM_STATE_SHOW:
		/* the followed car vanished/died — cut early (touring only) */
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

			if (s.leadMode)
			{
				/* keep the same rogue car; rotate the camera style */
				s.style = AntFarmPickStyle(1);
				s.trackPlaced = 0;
				AntFarmInitShotVars();
			}
			else
			{
				if (AntFarmPickFarArea())
				{
					s.spool = s.areaPos;
					s.targetPos = s.areaPos;
				}

				AntFarmPlanShot();
			}

			s.camSnapped = 0;	/* snap hidden by the black */
		}

		/* region streaming gate: wait until ControlMap has accepted the new
		 * spool (current_region matches the region the spool ACTUALLY points
		 * at this frame — after a car/road point is picked the spool can sit
		 * in a different region than the area anchor) AND the barrel regions
		 * are unpacked — never fade into an unloaded void. On a long timeout,
		 * re-pick a different area (up to a few tries), then proceed. */
		if (!s.leadMode)
		{
			int spoolRegion = AntFarmRegionOf(&s.spool);
			int regionsReady = (current_region == spoolRegion) && check_regions_present();

			if (!regionsReady && !s.streamDone)
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

						AntFarmPlanShot();
						s.camSnapped = 0;
					}
					else
					{
						/* best effort — every candidate area failed to stream */
						s.ctx->jer_log(s.ctx, "[antfarm] warning: area never streamed — proceeding\n");
						s.streamDone = 1;
						s.cutStart = now;
					}
				}

				/* hard total-black cap: never sit in the wash forever */
				if (now - s.cutEnter >= ANTFARM_BLACK_CAP_MS)
				{
					s.ctx->jer_log(s.ctx, "[antfarm] warning: black cap hit — proceeding\n");
					s.streamDone = 1;
					s.cutStart = now;
				}

				break;	/* keep the black */
			}

			if (!s.streamDone)
			{
				s.streamDone = 1;
				s.cutStart = now;	/* start the car-wait/hold clock now */
			}
		}

		/* car shots wait for traffic to populate the new area */
		if (!s.leadMode && s.cutWaitForCar)
		{
			int car = AntFarmPickCarNear(&s.areaPos, 14000);

			if (car >= 0)
			{
				s.targetCarId = car;
				s.cutWaitForCar = 0;
			}
			else if (now - s.cutStart >= ANTFARM_CAR_WAIT_MS)
			{
				/* quiet area — scenic road shot instead */
				s.targetKind = ANTFARM_TARGET_ROAD;
				s.style = AntFarmPickStyle(0);
				AntFarmSetupRoadShot();
				s.cutWaitForCar = 0;
			}
		}

		if (!s.cutWaitForCar && now - s.cutStart >= ANTFARM_CUT_HOLD_MS)
		{
			/* tiny chance for a rogue-car event (needs the player in a car so
			 * the cops have something to chase) */
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

			/* one-line transition log for diagnostics */
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

	return JER_RESULT_CONTINUE;
}

/* Pin the player's car to the camera focus while the screensaver runs:
 * teleport it there (so the region spool naturally follows the focus via
 * UpdatePlayers), reserve the slot and flip it to CONTROL_TYPE_NONE so it
 * is neither drawn nor simulated nor reusable. SFX are globally muted by
 * SetMasterVolume, so nothing else is needed for silence. */
static void AntFarmPinPlayerCar(void)
{
	int carId;
	CAR_DATA* cp;

	if (MainPlayer.playerType != PLAYER_TYPE_CAR)
		return;	/* on foot: the spool redirect covers streaming */

	carId = MainPlayer.playerCarId;

	if (carId < 0 || carId >= MAX_CARS)
		return;

	cp = &car_data[carId];

	/* never let the player car leave the playable world — if it fell out
	 * of bounds the game would end (the rogue car can overrun the map) */
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

	/* s.targetPos is the world-space focus (road anchor, followed car, or
	 * the rogue car during lead events) */
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

	if (cp->controlType != CONTROL_TYPE_NONE)
	{
		/* hide + take the slot out of the sim; reservedSlots stops the civ
		 * spawner (civ_ai.c:1724/1812/2014) from reusing it */
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
 * Positions are RENDER space; both helpers want WORLD space (Y negated —
 * camera.c:604 pattern; lineClear endpoints use MapHeight-style world Y,
 * cf. pathfind.c:503-508). */
static void AntFarmSceneryPass(VECTOR* camPos, const VECTOR* aim)
{
	VECTOR wa, wb;
	int arm = 256;	/* 256 = full camera arm retained */
	int i;
	static const int fracs[4] = { 192, 128, 80, 40 };

	/* world-space copies of the sight line */
	wa = *camPos;
	wa.vy = -wa.vy;
	wb = *aim;
	wb.vy = -wb.vy;

	/* (1) LOS pull-back */
	if (lineClear(&wa, &wb) == 0)
	{
		arm = fracs[3];	/* worst case: close shot */

		for (i = 0; i < 4; i++)
		{
			VECTOR test;

			test.vx = wb.vx + (wa.vx - wb.vx) * fracs[i] / 256;
			test.vy = wb.vy + (wa.vy - wb.vy) * fracs[i] / 256;
			test.vz = wb.vz + (wa.vz - wb.vz) * fracs[i] / 256;

			if (lineClear(&test, &wb) != 0)
			{
				arm = fracs[i];
				break;
			}
		}
	}

	/* smooth the pull so consecutive fractions blend instead of popping */
	s.armFrac += (arm - s.armFrac) * 30 / 100;

	if (s.armFrac > 256)
		s.armFrac = 256;

	arm = s.armFrac;

	camPos->vx = aim->vx + (camPos->vx - aim->vx) * arm / 256;
	camPos->vy = aim->vy + (camPos->vy - aim->vy) * arm / 256;
	camPos->vz = aim->vz + (camPos->vz - aim->vz) * arm / 256;

	/* (2) camera collider push-out (world space, exactly like the engine's
	 * chase cam at camera.c:588-616) */
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
}

/* render-time: take over the camera and keep the spool redirected.
 * MainPlayer.spoolXZ is re-pointed here (and not in the FRAME hook) because
 * UpdatePlayers() rewrites it to the player car every frame AFTER the world
 * step — ControlMap (next frame) reads whatever this hook left behind. */
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

	/* never aim at a zero/stale point — hold the last-good view instead */
	if (s.aimPos.vx == 0 && s.aimPos.vy == 0 && s.aimPos.vz == 0)
	{
		*(VECTOR*)a->cameraPosition = s.camPos;
		*(SVECTOR*)a->cameraAngle = s.camAngle;
		a->override = 1;
		return JER_RESULT_CONTINUE;
	}

	/* pin the player car to the focus (streaming follows it; hidden) */
	AntFarmPinPlayerCar();

	/* scenery: pull in for a clear sight-line, push out of buildings */
	AntFarmSceneryPass(&cam, &s.aimPos);

	/* never wander past the world edge into the nodraw skybox extremes */
	AntFarmClampToWorld(&cam);
	AntFarmClampToWorld(&s.aimPos);
	AntFarmClampAboveGround(&cam);
	AntFarmClampAboveGround(&s.aimPos);

	/* keep the smoothed state consistent so next frame eases from the
	 * corrected position, then re-aim at the unchanged focus */
	s.camPos = cam;

	PointAtTarget(&cam, &s.aimPos, &ang);
	s.camAngle = ang;

	*(VECTOR*)a->cameraPosition = cam;
	*(SVECTOR*)a->cameraAngle = ang;

	/* per-shot FOV (the engine re-seeds scr_z each frame; this is the last
	 * word before the draw — same pattern as d2pl's CAMERA hook) */
	SetGeomScreen(scr_z = s.shotScrZ);

	/* audio follows the filmed car ONLY on chase shots — the listener
	 * stays on the camera (gamesnd derives it from camera_position) so
	 * static/tripod/flyover sound fully spatialized around the shot */
	if (s.targetKind == ANTFARM_TARGET_CAR && s.targetCarId >= 0 &&
	    s.style == ANTFARM_STYLE_CHASE)
		CameraCar = s.targetCarId;

	a->override = 1;

	return JER_RESULT_CONTINUE;
}

/* on-foot input: explicit kill switch (in-car is covered by gStopPadReads) */
static int AntFarmOnPedInput(void* userdata, void* args)
{
	JER_ARGS_PED_INPUT* a = (JER_ARGS_PED_INPUT*)args;

	(void)userdata;

	if (s.active)
		a->pad = 0;

	return JER_RESULT_CONTINUE;
}

/* START during the screensaver: hand control back, then let the engine
 * pause open normally so the player can adjust/exit from the menu */
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

/* fade wash — drawn into the display buffer like the stock screen fade.
 * A single 50% semi-trans quad is only ever a grey veil (the void stays
 * visible through it); we stack an extra OPAQUE black quad at full fade so
 * the cut is truly black, never a see-through void. */
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
		/* opaque black on top of the wash — true black during the CUT */
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
/* pause menu (Pause -> Modules -> Ant Farm)                          */
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

	secs = AntIntervalMs() / 1000 + direction * 5;	/* 5s steps, 10..60 */

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
	s.intervalMs = ANTFARM_DEFAULT_INTERVAL * 1000;	/* valid before BOOT runs */

	ctx->jer_register_module(ctx,
		"antfarm",		/* id */
		"Ant Farm Screensaver",	/* name */
		"0.1.0",		/* version */
		"REDRIVER2 community",	/* author */
		"City-observer screensaver: diverse cinematic camera styles touring the whole map, with optional rogue-car chases.",	/* description */
		"",			/* dependencies */
		JERICHO_SDK_VERSION);

	ctx->jer_register_hook(ctx, JER_EVENT_BOOT, AntFarmOnBoot, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, AntFarmOnFrame, NULL, 0);
	/* CAMERA/PED_INPUT at a higher priority so the screensaver wins over
	 * other camera/pad modules (d2pl registers these at priority 0; the
	 * runtime runs higher priorities later, so our takeover is last word) */
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, AntFarmOnCamera, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, AntFarmOnPedInput, NULL, 10);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, AntFarmOnPauseMenu, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, AntFarmOnDrawOverlay, NULL, 0);

	jer_pause_menu_register(&antFarmMenu);

	ctx->jer_log(ctx, "[antfarm] registered (SDK v%d)\n", ctx->sdkVersion);
}
