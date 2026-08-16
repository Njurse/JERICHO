/*
 * antfarm.c — "Ant Farm" screensaver / idle mode (JERICHO module).
 *
 * A passive city observer for REDRIVER2. When enabled it:
 *   - cuts off player input (gStopPadReads — the car brakes and stays put,
 *     on-foot pads are zeroed) and hides the HUD (gDoOverlays = 0),
 *   - redirects the region spool (MainPlayer.spoolXZ) to the camera focus so
 *     map geometry + traffic stream to wherever the camera is looking,
 *   - cycles a cinematic camera between three shot types:
 *       ANTFARM_MODE_TRIPOD  : parked elevated camera watching traffic flow
 *       ANTFARM_MODE_FOLLOW  : chase-cam attached to a random moving civilian
 *       ANTFARM_MODE_FLYOVER : slow dolly/pan down a long straight
 *   - transitions with a soft fade (semi-transparent wash, same style as the
 *     stock FadeGameScreen) and reselects the target while the screen is
 *     fully washed so the new area streams in without popping.
 *
 * Toggle: F9 (keyboard, PC) or Pause -> Modules -> Ant Farm. Pressing START
 * during the screensaver hands control back and opens the normal pause.
 *
 * Pure JERICHO module — no game files are edited. Follows the d2pl /
 * sandbox module patterns (game headers, C linkage entry, pause menus,
 * persistent config).
 */

#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"
#include "jer_pause_menu.h"

#include "driver2.h"
#include "dr2math.h"
#include "players.h"	/* player[] / MainPlayer (spoolXZ) */
#include "cars.h"		/* car_data[] */
#include "camera.h"		/* camera_position, camera_angle, CameraCar, PointAtTarget */
#include "dr2roads.h"	/* Driver2StraightsPtr, MapHeight */
#include "main.h"		/* gStopPadReads, game_over, FrameCnt */
#include "overlay.h"	/* gDoOverlays */
#include "system.h"		/* current (prim buffer) */
#include "cutscene.h"	/* gInGameCutsceneActive */
#include "glaunch.h"	/* quick_replay, NoPlayerControl */
#include "mission.h"	/* NumPlayers */
#include "convert.h"	/* Random2 */
#include "objcoll.h"	/* lineClear, CheckScenaryCollisions */

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

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

typedef struct ANTFARM_STATE
{
	JERICHO_CONTEXT* ctx;

	int active;		/* screensaver running */
	int camSnapped;		/* camera has been snapped to the current target */

	int mode;		/* ANTFARM_MODE_* */
	int state;		/* ANTFARM_STATE_* */
	int fade;		/* 0..255 overlay wash intensity */
	unsigned long stateStart;	/* when the current state began (ms) */
	unsigned long shotStart;	/* when the current shot's fade-in began (ms) */

	int intervalMs;					/* seconds per visible shot */
	int modesEnabled[ANTFARM_MODE_COUNT];

	int targetCarId;	/* mode FOLLOW: car_data index */
	int targetRoadIdx;	/* modes TRIPOD/FLYOVER: straight index (== surfId) */
	VECTOR targetPos;	/* aim anchor — also the spool content for static shots */

	/* per-shot framing (modes TRIPOD/FLYOVER road reference) */
	int roadSurfId;		/* chosen straight's surfId (== index) */
	int roadLane;		/* lane index sampled on that road */
	int roadDist;		/* distAlongPath on the road (world units) */
	int shotSideSign;	/* +1/-1 — which side of the road the camera sits */
	int shotSideDist;	/* camera distance from the sampled lane point */
	int shotHeight;		/* camera elevation above the road */
	int shotLookAhead;	/* how far ahead of the camera the aim sits */
	int shotOrbitAmp;	/* tripod azimuth sweep amplitude (0 = static) */
	int shotOrbitPhase;	/* orbit phase seed */
	int shotScrZ;		/* per-shot FOV (projection distance) */
	int armFrac;		/* smoothed LOS pull-back fraction (256 = full arm) */

	VECTOR spool;		/* what MainPlayer.spoolXZ points at while active */

	VECTOR camPos;		/* smoothed camera position */
	VECTOR aimPos;		/* current aim point (render space) */
	SVECTOR camAngle;	/* smoothed camera angle */

	/* engine state saved/restored around activation */
	int savedStopPadReads;
	int savedDoOverlays;
	VECTOR* savedSpoolXZ;

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

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static CAR_DATA* AntFarmValidCar(void)
{
	if (s.targetCarId < 0 || s.targetCarId >= MAX_CARS)
		return NULL;

	CAR_DATA* cp = &car_data[s.targetCarId];

	if (cp->controlType != CONTROL_TYPE_CIV_AI)
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
static int AntFarmPickSurface(int minLen){
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

/* randomize the framing of a freshly picked shot so no two cuts look alike */
static void AntFarmInitShotVars(void)
{
	s.shotSideSign = (Random2(0) & 1) ? 1 : -1;
	s.shotSideDist = 620 + (Random2(0) % 380);	/* 620..1000 */
	s.shotLookAhead = 520 + (Random2(0) % 620);	/* 520..1140 */
	s.shotOrbitAmp = 0;
	s.armFrac = 256;
	s.shotOrbitPhase = AntTicks() & 4095;

	if (s.mode == ANTFARM_MODE_FLYOVER)
	{
		s.shotHeight = 430 + (Random2(0) % 240);	/* high, cinematic */
		s.shotScrZ = 218 + (Random2(0) % 24);		/* wider lens */
	}
	else if (s.mode == ANTFARM_MODE_FOLLOW)
	{
		s.shotHeight = 165 + (Random2(0) % 40);		/* chase height */
		s.shotScrZ = 240 + (Random2(0) % 20);
	}
	else	/* tripod */
	{
		s.shotHeight = 185 + (Random2(0) % 140);	/* 185..325 */
		s.shotScrZ = 240 + (Random2(0) % 24);

		/* roughly a third of tripod shots get a slow cinematic orbit */
		if (Random2(0) % 3 == 0)
			s.shotOrbitAmp = 384 + (Random2(0) % 384);	/* ±34°..±67° */
	}
}

/* count moving civilian cars (drivable traffic) */
static int AntFarmCountMovingCars(void)
{
	int i, n = 0;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_CIV_AI && ABS(cp->hd.speed) > 8)
			n++;
	}

	return n;
}

/* pick a random moving civilian car, or -1 when none */
static int AntFarmPickCar(void)
{
	int i, n = 0;
	int pick;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* cp = &car_data[i];

		if (cp->controlType == CONTROL_TYPE_CIV_AI && ABS(cp->hd.speed) > 8)
		{
			if (Random2(0) % (n + 1) == 0)	/* reservoir sampling */
				pick = i;
			n++;
		}
	}

	if (n == 0)
		return -1;

	return pick;
}

/* ------------------------------------------------------------------ */
/* target selection                                                   */
/* ------------------------------------------------------------------ */

static void AntFarmPickRoadShot(void);	/* defined below (needs the pickers) */

static void AntFarmPickTarget(void)
{
	int modeCandidates[ANTFARM_MODE_COUNT];
	int count = 0;
	int i;
	int pick;

	/* only modes that are both enabled AND have candidates right now */
	if (s.modesEnabled[ANTFARM_MODE_TRIPOD] && NumDriver2Straights > 0)
		modeCandidates[count++] = ANTFARM_MODE_TRIPOD;

	if (s.modesEnabled[ANTFARM_MODE_FLYOVER] && AntFarmPickSurface(4500) >= 0)
		modeCandidates[count++] = ANTFARM_MODE_FLYOVER;

	if (s.modesEnabled[ANTFARM_MODE_FOLLOW] && AntFarmCountMovingCars() > 0)
		modeCandidates[count++] = ANTFARM_MODE_FOLLOW;

	/* relax the availability filter if nothing was usable */
	if (count == 0)
	{
		if (NumDriver2Straights > 0)
			modeCandidates[count++] = ANTFARM_MODE_TRIPOD;

		if (AntFarmCountMovingCars() > 0)
			modeCandidates[count++] = ANTFARM_MODE_FOLLOW;

		if (NumDriver2Straights > 0)
			modeCandidates[count++] = ANTFARM_MODE_FLYOVER;
	}

	if (count == 0)
	{
		s.mode = ANTFARM_MODE_TRIPOD;
		s.targetRoadIdx = -1;
		s.targetCarId = -1;
		AntFarmInitShotVars();
		return;
	}

	pick = Random2(0) % count;
	s.mode = modeCandidates[pick];
	s.targetCarId = -1;
	s.targetRoadIdx = -1;

	if (s.mode == ANTFARM_MODE_FOLLOW)
	{
		CAR_DATA* cp;

		s.targetCarId = AntFarmPickCar();
		cp = AntFarmValidCar();

		if (cp)
		{
			s.targetPos.vx = cp->hd.where.t[0];
			s.targetPos.vy = cp->hd.where.t[1];
			s.targetPos.vz = cp->hd.where.t[2];
		}
		else
		{
			/* no car after all — degrade to a tripod */
			s.mode = ANTFARM_MODE_TRIPOD;
			s.targetCarId = -1;
			AntFarmPickRoadShot();
		}
	}
	else
	{
		AntFarmPickRoadShot();
	}

	AntFarmInitShotVars();

	/* diagnostics */
	s.ctx->jer_log(s.ctx, "[antfarm] cut #%d -> mode %s (%s%s)\n",
		++s.cutCount,
		s.mode == ANTFARM_MODE_TRIPOD ? "tripod" :
		s.mode == ANTFARM_MODE_FOLLOW ? "follow" : "flyover",
		s.mode == ANTFARM_MODE_FOLLOW ? "car" : "road",
		s.mode == ANTFARM_MODE_FOLLOW ? "" : " lane-based");
}

/* pick a concrete road + lane + distance for TRIPOD/FLYOVER shots and set
 * the spool anchor. Sampling near a straight's END frames junctions */
static void AntFarmPickRoadShot(void)
{
	DRIVER2_STRAIGHT* rd;
	int nLanes;
	int roll;
	int len;

	s.roadSurfId = AntFarmPickSurface(s.mode == ANTFARM_MODE_FLYOVER ? 4500 : 400);

	if (s.roadSurfId < 0)
	{
		s.targetPos.vx = camera_position.vx;
		s.targetPos.vy = camera_position.vy;
		s.targetPos.vz = camera_position.vz;
		return;
	}

	rd = &Driver2StraightsPtr[s.roadSurfId];
	len = rd->length;

	/* the sampled lane: random across the full carriageway */
	nLanes = ROAD_WIDTH_IN_LANES(rd);
	s.roadLane = (nLanes > 0) ? (Random2(0) % nLanes) : 0;

	/* where along the road the tripod stands: bias toward the ends so the
	 * shot sits beside an intersection (junctions have no centre of their
	 * own, but straight ends connect to them) */
	if (s.mode == ANTFARM_MODE_TRIPOD)
	{
		roll = Random2(0) % 100;

		if (roll < 30)
			s.roadDist = len - 350 - (Random2(0) % 600);	/* far end */
		else if (roll < 55)
			s.roadDist = 350 + (Random2(0) % 600);		/* near end */
		else
			s.roadDist = len / 2 + (Random2(0) % (len / 4)) - len / 8;	/* middle */
	}
	else
	{
		s.roadDist = 0;	/* flyover starts at one end of the road */
	}

	if (s.roadDist < 0)
		s.roadDist = 0;

	if (s.roadDist > len)
		s.roadDist = len;

	/* spool anchor = the sampled road point (world space; only x/z matter) */
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
			s.targetPos.vx = camera_position.vx;
			s.targetPos.vy = camera_position.vy;
			s.targetPos.vz = camera_position.vz;
		}
	}
}

/* ------------------------------------------------------------------ */
/* per-shot camera computation                                         */
/* ------------------------------------------------------------------ */

static void AntFarmComputeCamera(VECTOR* outPos, SVECTOR* outAngle)
{
	VECTOR desired = { 0, 0, 0 };
	VECTOR aim = { 0, 0, 0 };
	unsigned long now = AntTicks();
	int lerp;

	if (s.mode == ANTFARM_MODE_FOLLOW)
	{
		CAR_DATA* cp = AntFarmValidCar();
		VECTOR carPos;
		int dir, dist, h;

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

		dist = 560 + FIXEDH(ABS(cp->hd.speed)) / 3;

		if (dist > 900)
			dist = 900;

		/* render space is Y-flipped vs the world (camera.c:604 negates the
		 * camera Y for the world-space collider): a camera above the ground
		 * has a negative vy here. MapHeight()/hd.where.t[] use the world
		 * convention — negate Y for the render camera. */
		h = AntFarmMapHeight(carPos.vx, carPos.vz);

		desired.vx = carPos.vx + FIXEDH(RSIN((dir + 2048) & 0xfff) * dist);
		desired.vy = -h - s.shotHeight;
		desired.vz = carPos.vz + FIXEDH(RCOS((dir + 2048) & 0xfff) * dist);

		aim = carPos;
		aim.vy = -(carPos.vy + 60);

		/* keep the spool pinned to the moving car */
		s.spool = carPos;
		s.targetPos = carPos;

		lerp = 25;
	}
	else
	{
		VECTOR roadPt = { 0, 0, 0 };
		int heading = 0;

		/* re-resolve the road reference every frame (cheap) so the shot
		 * stays glued to the lane while the camera eases around it */
		if (AntFarmShotRoadRender(s.roadDist, &roadPt, &heading) == 0)
		{
			*outPos = s.camPos;
			*outAngle = s.camAngle;
			return;
		}

		if (s.mode == ANTFARM_MODE_TRIPOD)
		{
			/* elevated camera on the side of the road, watching the flow */
			int side = (heading + (s.shotSideSign > 0 ? 1024 : 3072)) & 0xfff;
			int sway = RSIN((now / 7) & 4095) >> 8;			/* -16..16 */
			int bobY = RSIN((now / 13) & 4095) >> 9;		/* -8..8 */

			if (s.shotOrbitAmp > 0)
			{
				/* slow cinematic orbit: one sweep per ~2 shot lengths */
				int sweepPhase = (s.shotOrbitPhase + now / (s.intervalMs * 2)) & 4095;
				int sweep = FIXEDH(RSIN(sweepPhase) * s.shotOrbitAmp);

				side = (side + 4096 + sweep) & 0xfff;
			}

			desired.vx = roadPt.vx + FIXEDH(RSIN(side) * s.shotSideDist) + sway;
			desired.vy = roadPt.vy - s.shotHeight + bobY;
			desired.vz = roadPt.vz + FIXEDH(RCOS(side) * s.shotSideDist);

			aim.vx = roadPt.vx + FIXEDH(RSIN(heading) * s.shotLookAhead);
			aim.vy = roadPt.vy - 40;
			aim.vz = roadPt.vz + FIXEDH(RCOS(heading) * s.shotLookAhead);
		}
		else	/* ANTFARM_MODE_FLYOVER */
		{
			/* slow dolly along the lane, high up, looking ahead */
			VECTOR startPt = { 0, 0, 0 };
			VECTOR endPt = { 0, 0, 0 };
			VECTOR aimPt = { 0, 0, 0 };
			int len = Driver2StraightsPtr[s.roadSurfId].length;
			int dummyHeading = 0;
			int t, tt, curDist, aimDist;
			unsigned long shotElapsed = now - s.shotStart;

			t = (shotElapsed >= (unsigned long)s.intervalMs) ? 1000
			    : (int)(shotElapsed * 1000 / (unsigned long)s.intervalMs);

			/* smoothstep — eases the dolly in and out of each end */
			tt = t * t * (3000 - 2 * t) / 1000000;	/* t,tt in 0..1000 */

			AntFarmShotRoadRender(0, &startPt, &dummyHeading);
			AntFarmShotRoadRender(len, &endPt, &dummyHeading);

			desired.vx = startPt.vx + (endPt.vx - startPt.vx) * tt / 1000;
			desired.vz = startPt.vz + (endPt.vz - startPt.vz) * tt / 1000;
			desired.vy = -(AntFarmMapHeight(desired.vx, desired.vz)) - s.shotHeight
				+ (RSIN((now / 11) & 4095) >> 6);

			/* aim ahead of the dolly, glued to the road */
			curDist = (long)len * tt / 1000;
			aimDist = curDist + s.shotLookAhead;

			if (aimDist > len)
				aimDist = len;

			AntFarmShotRoadRender(aimDist, &aimPt, &dummyHeading);
			aim = aimPt;
			aim.vy = aimPt.vy - 60;
		}

		/* static shots keep the spool on the road anchor */
		s.spool = s.targetPos;

		lerp = 14;
	}

	/* remember the aim for the scenery pass + re-aim */
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

		/* save + cut off input and HUD */
		s.savedStopPadReads = gStopPadReads;
		s.savedDoOverlays = gDoOverlays;
		s.savedSpoolXZ = MainPlayer.spoolXZ;

		gStopPadReads = 1;	/* player car brakes; on-foot pads zeroed */
		gDoOverlays = 0;	/* hide the HUD */

		s.active = 1;
		s.camSnapped = 0;
		s.fade = 0;
		s.state = ANTFARM_STATE_SHOW;
		s.stateStart = AntTicks();
		s.shotStart = s.stateStart;
		s.cutCount = 0;
		s.targetCarId = -1;
		s.targetRoadIdx = -1;

		s.camPos.vx = camera_position.vx;
		s.camPos.vy = camera_position.vy;
		s.camPos.vz = camera_position.vz;
		s.camAngle = camera_angle;

		/* start from where the player was looking */
		s.targetPos = s.camPos;

		AntFarmPickTarget();

		s.ctx->jer_log(s.ctx,
			"[antfarm] enabled (interval %ds, tripod=%d follow=%d flyover=%d)\n",
			s.intervalMs / 1000,
			s.modesEnabled[ANTFARM_MODE_TRIPOD],
			s.modesEnabled[ANTFARM_MODE_FOLLOW],
			s.modesEnabled[ANTFARM_MODE_FLYOVER]);
	}
	else
	{
		gStopPadReads = s.savedStopPadReads;
		gDoOverlays = s.savedDoOverlays;

		/* restore the spool pointer (UpdatePlayers would re-point it to the
		 * player car next frame anyway — restoring keeps the contract exact) */
		MainPlayer.spoolXZ = s.savedSpoolXZ;

		s.active = 0;
		s.fade = 0;

		s.ctx->jer_log(s.ctx,
			"[antfarm] disabled (cuts: %d; restored pads=%d overlays=%d)\n",
			s.cutCount, s.savedStopPadReads, s.savedDoOverlays);
	}
}

static void AntFarmToggle(void)
{
	AntFarmSetActive(s.active ? 0 : 1);
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

/* every world step: F9 toggle, cut state machine, follow-car guard */
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

	switch (s.state)
	{
	case ANTFARM_STATE_SHOW:
		/* the followed car vanished/died — cut early */
		if (s.mode == ANTFARM_MODE_FOLLOW && AntFarmValidCar() == NULL)
		{
			s.state = ANTFARM_STATE_FADE_OUT;
			s.stateStart = now;
			break;
		}

		if (now - s.stateStart >= (unsigned long)s.intervalMs)
		{
			s.state = ANTFARM_STATE_FADE_OUT;
			s.stateStart = now;
		}
		break;

	case ANTFARM_STATE_FADE_OUT:
		s.fade = (int)((now - s.stateStart) * 255 / ANTFARM_FADE_MS);

		if (s.fade >= 255)
		{
			AntFarmPickTarget();
			s.camSnapped = 0;	/* snap hidden by the black */
			s.fade = 255;
			s.state = ANTFARM_STATE_CUT;
			s.stateStart = now;
		}
		break;

	case ANTFARM_STATE_CUT:
		/* hold the black while regions/traffic stream to the new focus */
		if (now - s.stateStart >= ANTFARM_CUT_HOLD_MS)
		{
			s.state = ANTFARM_STATE_FADE_IN;
			s.stateStart = now;
			s.shotStart = now;
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

	/* scenery: pull in for a clear sight-line, push out of buildings */
	AntFarmSceneryPass(&cam, &s.aimPos);

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

	/* audio follows the filmed car when chasing one */
	if (s.mode == ANTFARM_MODE_FOLLOW && s.targetCarId >= 0)
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
		AntFarmSetActive(0);

	return JER_RESULT_CONTINUE;
}

/* fade wash — drawn into the display buffer like the stock screen fade */
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

	snprintf(out, max, "Cut interval: %ds", s.intervalMs / 1000);
}

static int AntIntervalAdjust(void* userdata, int direction)
{
	int secs;

	(void)userdata;

	secs = s.intervalMs / 1000 + direction;

	if (secs < ANTFARM_MIN_INTERVAL)
		secs = ANTFARM_MIN_INTERVAL;

	if (secs > ANTFARM_MAX_INTERVAL)
		secs = ANTFARM_MAX_INTERVAL;

	s.intervalMs = secs * 1000;
	jer_config_set_int(ANT_MOD_ID, "interval", secs);

	return JER_PAUSE_QUIT_NONE;
}

static void AntModeLabel(int mode, const char* name, char* out, int max)
{
	snprintf(out, max, "%s: %s", name, s.modesEnabled[mode] ? "ON" : "OFF");
}

static int AntModeToggle(int mode, const char* key)
{
	s.modesEnabled[mode] = !s.modesEnabled[mode];
	jer_config_set_bool(ANT_MOD_ID, key, s.modesEnabled[mode]);

	return JER_PAUSE_QUIT_NONE;
}

static void AntTripodLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntModeLabel(ANTFARM_MODE_TRIPOD, "Junction", out, max);
}

static int AntTripodToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntModeToggle(ANTFARM_MODE_TRIPOD, "mode_tripod");
}

static void AntFollowLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntModeLabel(ANTFARM_MODE_FOLLOW, "Car follow", out, max);
}

static int AntFollowToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntModeToggle(ANTFARM_MODE_FOLLOW, "mode_follow");
}

static void AntFlyoverLabel(void* userdata, char* out, int max)
{
	(void)userdata;
	AntModeLabel(ANTFARM_MODE_FLYOVER, "Flyover", out, max);
}

static int AntFlyoverToggle(void* userdata, int direction)
{
	(void)userdata;
	(void)direction;
	return AntModeToggle(ANTFARM_MODE_FLYOVER, "mode_flyover");
}

static const JER_PAUSE_MENU_ITEM antSettingsItems[] = {
	{ NULL, AntIntervalLabel, AntIntervalAdjust, NULL, NULL, 1 },
	{ NULL, AntTripodLabel,   AntTripodToggle,   NULL, NULL, 0 },
	{ NULL, AntFollowLabel,   AntFollowToggle,   NULL, NULL, 0 },
	{ NULL, AntFlyoverLabel,  AntFlyoverToggle,  NULL, NULL, 0 },
};

static const JER_PAUSE_MENU antSettingsMenu = { "Ant Farm Settings", antSettingsItems, 4 };

static const JER_PAUSE_MENU_ITEM antMenuItems[] = {
	{ NULL, AntMenuLabel, AntMenuToggle, NULL, NULL, 0 },
	{ "Settings", NULL, NULL, NULL, &antSettingsMenu, 0 },
};

static const JER_PAUSE_MENU antFarmMenu = { "Ant Farm", antMenuItems, 2 };

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
	s.modesEnabled[ANTFARM_MODE_TRIPOD] = jer_config_get_bool(ANT_MOD_ID, "mode_tripod", 1);
	s.modesEnabled[ANTFARM_MODE_FOLLOW] = jer_config_get_bool(ANT_MOD_ID, "mode_follow", 1);
	s.modesEnabled[ANTFARM_MODE_FLYOVER] = jer_config_get_bool(ANT_MOD_ID, "mode_flyover", 1);

	s.ctx->jer_log(s.ctx,
		"[antfarm] ready: interval %ds, tripod=%d follow=%d flyover=%d\n",
		s.intervalMs / 1000,
		s.modesEnabled[ANTFARM_MODE_TRIPOD],
		s.modesEnabled[ANTFARM_MODE_FOLLOW],
		s.modesEnabled[ANTFARM_MODE_FLYOVER]);

	return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_antfarm_entry)(JERICHO_CONTEXT* ctx)
{
	memset(&s, 0, sizeof(s));
	s.ctx = ctx;

	ctx->jer_register_module(ctx,
		"antfarm",		/* id */
		"Ant Farm Screensaver",	/* name */
		"0.1.0",		/* version */
		"REDRIVER2 community",	/* author */
		"Passive city observer: cycling tripod/follow/flyover camera cuts with gentle fades.",	/* description */
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
