/*
 * d2pl.c — Driver 2 Parallel Lines (d2pl): the big weapon + camera mod.
 *
 * Merges the former camera + weapons packages into one module so the
 * free-look camera, the GTA-style on-foot/in-car framing, and the weapon
 * system share one settings store and never fight each other.
 *
 * What this module does:
 *   camera — right-stick orbit + pitch free-look (dual-stick TPS style),
 *            smooth settle-back, pitch-linked FOV, configurable base FOV
 *            override (~72 degrees), camera-relative on-foot movement with
 *            run/pivot turn limits, GTA4-style in-car framing scaled to the
 *            car's bounding box, low-speed orbit grip (no auto camera swing
 *            at a standstill), configurable sensitivity/invert/offsets.
 *   weapons — overridable WeaponBase template + registry, arm-hold (pose
 *            the ped arm, attach the mesh at RHAND), L1 aim (zoom + FOV +
 *            presenting pose), R1/R2 fire, ammo/reload, HUD, laser sight
 *            (color-cycled, default red), 1-second impact marker on fire.
 *
 * All tunables live in mods/config/d2pl.ini (see the settings block below)
 * and are editable from Pause -> D2PL Settings.
 *
 * Hook usage:
 *   JER_EVENT_CAMERA_LOOK  — right-stick free-look + aim look suppression
 *   JER_EVENT_CAMERA       — framing offsets + FOV override + aim camera
 *   JER_EVENT_PED_INPUT    — camera-relative on-foot movement
 *   JER_EVENT_PED_SKELETON — arm pose + weapon mesh at RHAND
 *   JER_EVENT_FRAME        — weapon update + laser line + impact marker
 *   JER_EVENT_DRAW_OVERLAY — HUD (weapon name + ammo)
 *   JER_EVENT_PAUSE_MENU   — D2PL Settings bridge (labels + adjust)
 *   JER_EVENT_GAME_START / JER_EVENT_INIT — reset per-level state
 */
#include "jericho.h"
#include "jer_events.h"
#include "jer_config.h"

#include "d2pl.h"

#include "driver2.h"
#include "players.h"
#include "cars.h"
#include "camera.h"
#include "pad.h"
#include "dr2math.h"
#include "main.h"
#include "draw.h"
#include "models.h"
#include "pres.h"
#include "PsyX/PsyX_public.h"	/* PsyX_GetScreenSize (FOV math) */

#include <math.h>

/* the debug line API used by the engine's collision overlays (PC only) */
#ifndef PSX
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

/* ================================================================== */
/* Settings (persisted to mods/config/d2pl.ini)                       */
/* ================================================================== */

typedef struct D2PL_SETTINGS
{
	int sensX;		/* view-change sensitivity, horizontal (64 = default) */
	int sensY;		/* view-change sensitivity, vertical (64 = default) */
	int footDist;		/* on-foot camera pull-in distance */
	int footLat;		/* on-foot shoulder offset */
	int carDist;		/* in-car pull-in (fraction of the view axis) */
	int carLatPct;		/* in-car lateral, % of the car's bbox width */
	int shoulder;		/* -1 = left shoulder, +1 = right */
	int invertH;		/* 1 = invert horizontal look */
	int invertV;		/* 1 = invert vertical look */
	int fovEnabled;		/* 1 = override the engine FOV */
	int fovDeg;		/* target horizontal FOV (degrees) */
	int laserColor;		/* D2PL_LASER_* index (0 = off) */
} D2PL_SETTINGS;

#define D2PL_CFG_MOD "d2pl"

static D2PL_SETTINGS gS = {
	64,			/* sensX */
	64,			/* sensY */
	400,			/* footDist */
	170,			/* footLat */
	500,			/* carDist */
	28,			/* carLatPct */
	-1,			/* shoulder (left) */
	0,			/* invertH */
	0,			/* invertV */
	1,			/* fovEnabled */
	72,			/* fovDeg */
	D2PL_LASER_DEFAULT	/* laserColor (red) */
};

static void D2plLoadSettings(void)
{
	gS.sensX = jer_config_get_int(D2PL_CFG_MOD, "sens_x", gS.sensX);
	gS.sensY = jer_config_get_int(D2PL_CFG_MOD, "sens_y", gS.sensY);
	gS.footDist = jer_config_get_int(D2PL_CFG_MOD, "foot_dist", gS.footDist);
	gS.footLat = jer_config_get_int(D2PL_CFG_MOD, "foot_lat", gS.footLat);
	gS.carDist = jer_config_get_int(D2PL_CFG_MOD, "car_dist", gS.carDist);
	gS.carLatPct = jer_config_get_int(D2PL_CFG_MOD, "car_lat_pct", gS.carLatPct);
	gS.shoulder = jer_config_get_int(D2PL_CFG_MOD, "shoulder", gS.shoulder);
	gS.invertH = jer_config_get_int(D2PL_CFG_MOD, "invert_h", gS.invertH);
	gS.invertV = jer_config_get_int(D2PL_CFG_MOD, "invert_v", gS.invertV);
	gS.fovEnabled = jer_config_get_int(D2PL_CFG_MOD, "fov_enabled", gS.fovEnabled);
	gS.fovDeg = jer_config_get_int(D2PL_CFG_MOD, "fov_deg", gS.fovDeg);
	gS.laserColor = jer_config_get_int(D2PL_CFG_MOD, "laser_color", gS.laserColor);

	/* clamp so a hand-edited file can't break the camera */
	gS.sensX = jer_clamp_int(gS.sensX, 16, 160);
	gS.sensY = jer_clamp_int(gS.sensY, 16, 160);
	gS.footDist = jer_clamp_int(gS.footDist, 120, 900);
	gS.footLat = jer_clamp_int(gS.footLat, 0, 500);
	gS.carDist = jer_clamp_int(gS.carDist, 100, 1000);
	gS.carLatPct = jer_clamp_int(gS.carLatPct, 5, 60);
	gS.shoulder = (gS.shoulder > 0) ? 1 : -1;
	gS.fovDeg = jer_clamp_int(gS.fovDeg, 55, 90);
	gS.laserColor = (gS.laserColor < 0 || gS.laserColor >= D2PL_LASER_COUNT)
		? D2PL_LASER_DEFAULT : gS.laserColor;
}

static void D2plSaveSettings(void)
{
	jer_config_set_int(D2PL_CFG_MOD, "sens_x", gS.sensX);
	jer_config_set_int(D2PL_CFG_MOD, "sens_y", gS.sensY);
	jer_config_set_int(D2PL_CFG_MOD, "foot_dist", gS.footDist);
	jer_config_set_int(D2PL_CFG_MOD, "foot_lat", gS.footLat);
	jer_config_set_int(D2PL_CFG_MOD, "car_dist", gS.carDist);
	jer_config_set_int(D2PL_CFG_MOD, "car_lat_pct", gS.carLatPct);
	jer_config_set_int(D2PL_CFG_MOD, "shoulder", gS.shoulder);
	jer_config_set_int(D2PL_CFG_MOD, "invert_h", gS.invertH);
	jer_config_set_int(D2PL_CFG_MOD, "invert_v", gS.invertV);
	jer_config_set_int(D2PL_CFG_MOD, "fov_enabled", gS.fovEnabled);
	jer_config_set_int(D2PL_CFG_MOD, "fov_deg", gS.fovDeg);
	jer_config_set_int(D2PL_CFG_MOD, "laser_color", gS.laserColor);
}

/* ================================================================== */
/* Camera tuning constants                                             */
/* ================================================================== */

#define LOOK_DEADZONE 24	/* stick counts below this are "idle" */
#define LOOK_YAW_SIGN -1	/* -1 = push right orbits the camera right */
#define YAW_RATE 64		/* orbit speed at full stick, sensX = 64 */
#define YAW_SMOOTH 4		/* eased yaw: 1/div of the way to the stick target
				   per frame (smooth start/stop, not linear) */
#define LOOK_PITCH_SIGN 1	/* +1 = push down pitches down (vx up) */
#define PITCH_MAX 700		/* ~61 degrees of pitch up/down each way —
				   a proper TPS camera never points straight up/down */
#define FOV_PITCH_SCALE 8	/* scr_z delta per 128 pitch units (GTA4-style:
				   looking down narrows the FOV, up widens it) */
#define LOOK_SETTLE_FOOT 100	/* frames idle before settling back (on foot) */
#define LOOK_SETTLE_CAR 45	/* cars settle back sooner */
#define LOW_SPEED_GRACE 4	/* car wheel_speed at/below which the orbit stays
				   gripped (no camera swing at a standstill) */

#define MOVE_DEADZONE 24	/* left-stick deadzone */
#define MOVE_TURN_SIGN 1	/* +1 = positive angle delta turns right */
#define MOVE_STICK_SIGN_X 1	/* flip if your pad's X axis is inverted */
#define MOVE_STICK_SIGN_Y 1	/* flip if your pad's Y axis is inverted */
#define RUN_TURN_LIMIT 32	/* max turn per frame while moving (~2.8 deg) */
#define PIVOT_TURN_LIMIT 256	/* standstill pivot speed (~22.5 deg/frame) */

#define CAR_HEIGHT -60		/* in-car camera drop (more grounded) */
#define CAM_HEIGHT -110		/* on-foot camera height (shoulder level) */
#define CAR_SPEED_DIV 2		/* car pull-in fades with speed (GTA4 zoom-out) */

/* aim camera (on foot) */
#define AIM_PULL 2000		/* /4096: pull toward Tanner while aiming */
#define AIM_HEIGHT -60
#define AIM_ZOOM 160		/* scr_z increase -> FOV decrease (focus) */
#define AIM_LATERAL 90		/* shoulder bias while aiming */

/* impact marker: a big black X at the aim point, 1 second (60 frames) */
#define MARKER_LIFETIME 60
#define MARKER_MIN_SIZE 150
#define MARKER_MAX_SIZE 800

/* ================================================================== */
/* Module state                                                        */
/* ================================================================== */

#define MAX_WEAPONS 8

/* free-look state (reset per level) */
static int gLookIdle;		/* frames since the stick was last used */
static int gLookPitch;		/* smoothed pitch */
static int gLookPitchTarget;

/* ped was moving last frame (run-vs-pivot turn limit) */
static int gPedWasMoving;

/* weapon state */
static WeaponBase* gWeapons[MAX_WEAPONS];
static int gWeaponCount = 0;
static WeaponBase* gCurrentWeapon = NULL;
static int gAiming = 0;
static int gHandPos[3];		/* world RHAND position (muzzle origin) */
static int gAimPoint[3];	/* world aim point at weapon range */

/* impact marker state */
static int gMarkerTimer;	/* frames left until the marker fades */
static int gMarkerPos[3];

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static int D2plCarSpeed(PLAYER* lp)
{
	if (lp->playerCarId < 0)
		return 0;

	return ABS(FIXEDH(car_data[lp->playerCarId].hd.wheel_speed));
}

/* scr_z that yields `degrees` of horizontal FOV at the current aspect
 * ratio, inverting the engine's FrAng = ratan2(160, scr_z * aspect * 1.35) */
static int D2plFovScrZ(int degrees)
{
	int w = 640;
	int h = 480;
	float halfRad;
	float scrz;

	PsyX_GetScreenSize(&w, &h);

	if (w <= 0 || h <= 0)
	{
		w = 640;
		h = 480;
	}

	halfRad = (float)degrees * 3.14159265f / 360.0f;
	scrz = 160.0f / (tanf(halfRad) * ((float)h / (float)w) * 1.35f);

	if (scrz < 96.0f)
		scrz = 96.0f;
	if (scrz > 768.0f)
		scrz = 768.0f;

	return (int)scrz;
}

/* laser color for the current setting (index into the CVECTOR table) */
static void D2plLaserColor(CVECTOR* out)
{
	switch (gS.laserColor)
	{
	case D2PL_LASER_GREEN:
		out->r = 0; out->g = 250; out->b = 0; break;
	case D2PL_LASER_BLUE:
		out->r = 0; out->g = 90; out->b = 250; break;
	case D2PL_LASER_WHITE:
		out->r = 250; out->g = 250; out->b = 250; break;
	case D2PL_LASER_RED:
	default:
		out->r = 250; out->g = 0; out->b = 0; break;
	}
}

/* ================================================================== */
/* Weapon registry                                                     */
/* ================================================================== */

void WeaponSystem_Register(WeaponBase* weapon)
{
	if (weapon == NULL || gWeaponCount >= MAX_WEAPONS)
		return;

	gWeapons[gWeaponCount++] = weapon;
}

void WeaponSystem_Equip(WeaponBase* weapon)
{
	if (gCurrentWeapon == weapon)
		return;

	if (gCurrentWeapon != NULL)
		gCurrentWeapon->onHolster();

	gCurrentWeapon = weapon;

	if (weapon != NULL)
		weapon->onEquip();
}

void WeaponSystem_Cycle(int direction)
{
	int i;

	if (gWeaponCount == 0)
		return;

	if (gCurrentWeapon == NULL)
	{
		WeaponSystem_Equip(gWeapons[0]);
		return;
	}

	for (i = 0; i < gWeaponCount; i++)
	{
		if (gWeapons[i] == gCurrentWeapon)
			break;
	}

	if (i >= gWeaponCount)
		i = 0;

	i = (i + direction + gWeaponCount) % gWeaponCount;
	WeaponSystem_Equip(gWeapons[i]);
}

WeaponBase* WeaponSystem_Current(void)
{
	return gCurrentWeapon;
}

int* WeaponSystem_HandPos(void)
{
	return gHandPos;
}

/* ================================================================== */
/* JER_EVENT_CAMERA_LOOK — right-stick orbit + pitch free-look         */
/* ================================================================== */

static int D2plOnLook(void* userdata, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	PLAYER* lp = (PLAYER*)a->player;
	int inCar = (lp->playerCarId >= 0);
	int stickX = a->stickX;
	int stickY = a->stickY;
	int usingStick;
	int stockLook;

	(void)userdata;

	if (lp->padid < 0)
		return JER_RESULT_CONTINUE;

	/* while aiming the L2/R2 stock look buttons must not fire */
	if (gAiming)
		a->suppress = 1;

	usingStick = (ABS(stickX) > LOOK_DEADZONE || ABS(stickY) > LOOK_DEADZONE);
	stockLook = (a->paddCamera & (CAMERA_PAD_LOOK_LEFT | CAMERA_PAD_LOOK_RIGHT |
		CAMERA_PAD_LOOK_BACK | CAMERA_PAD_LOOK_BACK_DED)) != 0;

	if (usingStick)
	{
		int yawRate = (YAW_RATE * gS.sensX) / 64;
		int pitchMax = (PITCH_MAX * gS.sensY) / 64;
		int sX = gS.invertH ? -stickX : stickX;
		int sY = gS.invertV ? -stickY : stickY;

		/* orbit: drive the camera orbit angle toward the stick's target and
		 * EASE the motion (jer_lerp_angle) instead of stepping it linearly,
		 * so the view accelerates into/out of a change in look input. The
		 * orbit stays gripped so the engine's settle-back lerp can't fight
		 * it; vertical is clamped below. */
		gLookIdle = 0;
		a->suppress = 1;
		a->gripOrbit = 1;

		{
			int target = (lp->cameraAngle + (LOOK_YAW_SIGN * sX * yawRate) / 128) & 0xfff;

			lp->cameraAngle = jer_lerp_angle(lp->cameraAngle, target, YAW_SMOOTH);
		}

		/* clamp so the camera can never swing past straight up/down even at
		 * the highest sensitivity setting */
		pitchMax = jer_clamp_int(pitchMax, 0, 900);

		gLookPitchTarget = jer_clamp_int(
			(LOOK_PITCH_SIGN * sY * pitchMax) / 128, -pitchMax, pitchMax);
	}
	else if (stockLook)
	{
		/* yield to the stock look buttons */
		gLookIdle = 0;
		a->gripOrbit = 0;
	}
	else
	{
		gLookIdle++;

		if (gLookIdle > (inCar ? LOOK_SETTLE_CAR : LOOK_SETTLE_FOOT))
		{
			/* released: settle back behind the player — EXCEPT at a
			 * standstill in a car, where the camera holds still (reverse
			 * grace: no auto swing unless the player is deliberately
			 * moving backward) */
			if (inCar && D2plCarSpeed(lp) <= LOW_SPEED_GRACE)
			{
				a->suppress = 1;
				a->gripOrbit = 1;
			}
			else
			{
				a->gripOrbit = 0;
				gLookPitchTarget = 0;
			}
		}
		else
		{
			/* still inside the hold window: keep the orbit + pitch */
			a->suppress = 1;
			a->gripOrbit = 1;
		}
	}

	/* smooth the pitch toward its target (yaw is applied directly) */
	gLookPitch = jer_lerp_int(gLookPitch, gLookPitchTarget, 4);

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_PED_INPUT — camera-relative on-foot movement              */
/* ================================================================== */

static int D2plOnPedInput(void* userdata, void* args)
{
	JER_ARGS_PED_INPUT* a = (JER_ARGS_PED_INPUT*)args;
	PLAYER* lp = (PLAYER*)a->player;
	int stickX = a->stickX;
	int stickY = a->stickY;
	int fx, fz;
	int camHeading;
	int stickHeading;
	int desired;
	int delta;
	int limit;
	int mag;

	(void)userdata;

	if (lp->padid < 0 || lp->pPed == NULL)
		return JER_RESULT_CONTINUE;

	mag = ABS(stickX) + ABS(stickY);

	if (mag <= MOVE_DEADZONE)
	{
		gPedWasMoving = 0;
		return JER_RESULT_CONTINUE;	/* stock handling */
	}

	/* where the camera looks, as a world heading (movement convention:
	 * heading h moves along (RSIN h, RCOS h)) — direction from the camera
	 * to Tanner, the same vector the chase cam aims along */
	fx = lp->pos[0] - camera_position.vx;
	fz = lp->pos[2] - camera_position.vz;

	if (fx == 0 && fz == 0)
		return JER_RESULT_CONTINUE;

	camHeading = ratan2(fx, fz);

	/* stick direction relative to the screen: up = forward, right = right */
	stickHeading = ratan2(MOVE_STICK_SIGN_X * stickX, MOVE_STICK_SIGN_Y * -stickY);

	desired = (camHeading + stickHeading) & 0xfff;
	delta = DIFF_ANGLES(lp->dir, desired) * MOVE_TURN_SIGN;

	/* interpolate the heading toward the target: while running the turn is
	 * limited (can't snap), from standstill the pivot is much quicker */
	limit = (gPedWasMoving || ABS(lp->pPed->speed) > 4) ? RUN_TURN_LIMIT : PIVOT_TURN_LIMIT;
	delta = jer_clamp_int(delta, -limit, limit);

	lp->dir = (lp->dir + delta) & 0xfff;
	lp->pPed->dir.vy = (lp->dir + 2048) & 0xfff;

	/* rewrite the movement bits: clear the engine's D-pad synth + tank
	 * steer, then apply camera-relative forward/back */
	a->pad &= ~(TANNER_PAD_GOFORWARD | TANNER_PAD_GOBACK |
		TANNER_PAD_TURNLEFT | TANNER_PAD_TURNRIGHT);

	if (MOVE_STICK_SIGN_Y * stickY > MOVE_DEADZONE && ABS(stickX) <= MOVE_DEADZONE)
		a->pad |= TANNER_PAD_GOBACK;	/* pulled straight back: walk back */
	else
		a->pad |= TANNER_PAD_GOFORWARD;	/* otherwise walk forward */

	gPedWasMoving = (a->pad & (TANNER_PAD_GOFORWARD | TANNER_PAD_GOBACK)) != 0;

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_CAMERA — framing + FOV override + aim camera              */
/* ================================================================== */

static int D2plOnCamera(void* userdata, void* args)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	PLAYER* lp = (PLAYER*)a->player;
	VECTOR* camPos = (VECTOR*)a->cameraPosition;
	SVECTOR* camAngle = (SVECTOR*)a->cameraAngle;
	int heading;

	(void)userdata;

	/* chase camera only */
	if (a->cameraView != 0)
		return JER_RESULT_CONTINUE;

	/* view-relative heading: the direction the camera looks */
	heading = (-camAngle->vy) & 0xfff;

	if (gAiming && gCurrentWeapon != NULL && lp->playerCarId < 0)
	{
		/* --- aiming: "orbit looking OUTWARD" — the view tilts with the
		 * stick (camera-relative aim, crosshair centered on screen) and the
		 * player sits over the shoulder; conventional shooter behavior --- */
		camAngle->vx += gLookPitch;

		camPos->vx += (lp->pos[0] - camPos->vx) * AIM_PULL / 4096;
		camPos->vz += (lp->pos[2] - camPos->vz) * AIM_PULL / 4096;

		camPos->vy += AIM_HEIGHT;

		camPos->vx += FIXEDH(RCOS(heading) * AIM_LATERAL * gS.shoulder);
		camPos->vz += -FIXEDH(RSIN(heading) * AIM_LATERAL * gS.shoulder);

		SetGeomScreen(scr_z = gCameraDefaultScrZ + AIM_ZOOM);

		gCurrentWeapon->aimPoint(gAimPoint);

		a->override = 1;

		return JER_RESULT_CONTINUE;
	}

	/* --- neutral camera: base FOV override + pitch link. The pitch is the
	 * camera's ELEVATION around the player (up lifts the camera, so the
	 * view looks DOWN at the player from above); looking down from above
	 * narrows the FOV (GTA4-style), looking up from below widens it. --- */
	{
		int fovScrZ = gS.fovEnabled ? D2plFovScrZ(gS.fovDeg) : gCameraDefaultScrZ;

		SetGeomScreen(scr_z = fovScrZ - (gLookPitch * FOV_PITCH_SCALE) / 128);
	}

	if (lp->playerCarId >= 0)
	{
		/* --- GTA4-style chase cam for vehicles --- */
		CAR_COSMETICS* car_cos = car_data[lp->playerCarId].ap.carCos;
		int lateral = 120;
		int speed = D2plCarSpeed(lp);
		int pull = (gS.carDist * 2048) / (2048 + speed * CAR_SPEED_DIV);

		if (pull < 100)
			pull = 100;

		if (car_cos != NULL)
		{
			lateral = jer_clamp_int(
				(car_cos->colBox.vx * 2 * gS.carLatPct) / 100, 60, 320);
		}

		camPos->vx += (lp->pos[0] - camPos->vx) * pull / 4096;
		camPos->vz += (lp->pos[2] - camPos->vz) * pull / 4096;

		camPos->vy += CAR_HEIGHT;

		camPos->vx += FIXEDH(RCOS(heading) * lateral * gS.shoulder);
		camPos->vz += -FIXEDH(RSIN(heading) * lateral * gS.shoulder);
	}
	else
	{
		/* --- on foot: pull in + shoulder shift, rotating with the view --- */
		camPos->vx += -FIXEDH(RSIN(heading) * gS.footDist)
			+ FIXEDH(RCOS(heading) * gS.footLat * gS.shoulder);
		camPos->vz += -FIXEDH(RCOS(heading) * gS.footDist)
			- FIXEDH(RSIN(heading) * gS.footLat * gS.shoulder);

		camPos->vy += CAM_HEIGHT;

		/* subtle view bob/sway while Tanner moves (still when idle) */
		{
			static int lastX;
			static int lastZ;
			int speed = ABS(lp->pos[0] - lastX) + ABS(lp->pos[2] - lastZ);

			lastX = lp->pos[0];
			lastZ = lp->pos[2];

			if (speed > 8)
			{
				camPos->vy += FIXEDH(RSIN(FrameCnt * 3) * 40);
				camPos->vx += FIXEDH(RCOS(FrameCnt * 2) * 24);
			}
		}
	}

	/* --- "orbit looking INWARD": pushing up LIFTS the camera up around the
	 * player/vehicle (a true sphere orbit — elevation), so you see them from
	 * above; pushing down drops it below them. The offset vector is rotated
	 * by the elevation angle in the vertical plane containing the view, and
	 * the view is re-tilted by the *change* in the true down-angle to the
	 * target — the focus point (the player) never moves off-center. --- */
	if (gLookPitch != 0 && a->basePos != NULL)
	{
		int* base = (int*)a->basePos;
		int ox = camPos->vx - base[0];
		int oz = camPos->vz - base[2];
		int oy = camPos->vy - base[1];
		int cosP = RCOS(gLookPitch);		/* cos(elevation): 0..4096 */
		int sinP = RSIN(gLookPitch);		/* -sin(elevation) */
		int distH = SquareRoot0(ox * ox + oz * oz);
		int aim0;
		int aim1;

		if (distH > 0)
		{
			/* true down-angle to the target before and after the orbit */
			aim0 = ratan2(camPos->vy - base[1], distH);

			/* rotate the offset (ox, oy, oz) around the horizontal axis by
			 * the elevation: h' = h*cosP + oy*sinP, oy' = oy*cosP - distH*sinP.
			 * Sphere rotation — the distance to the player never changes. */
			{
				int hShift = (oy * sinP) >> 12;	/* vertical -> horizontal spill */

				camPos->vx = base[0] + FIXEDH(ox * cosP) + (hShift * ox) / distH;
				camPos->vz = base[2] + FIXEDH(oz * cosP) + (hShift * oz) / distH;
				camPos->vy = base[1] + FIXEDH(oy * cosP) - FIXEDH(distH * sinP);
			}

			aim1 = ratan2(camPos->vy - base[1], SquareRoot0(
				(camPos->vx - base[0]) * (camPos->vx - base[0]) +
				(camPos->vz - base[2]) * (camPos->vz - base[2])));

			/* add only the pitch-induced change to whatever the engine set
			 * (25 on foot, terrain-clamped in cars) so the rest pose stays
			 * identical and the transition is continuous */
			camAngle->vx += (aim1 - aim0);
		}
	}

	a->override = 1;

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_PED_SKELETON — arm pose + weapon mesh at RHAND            */
/* ================================================================== */

static int D2plOnSkeleton(void* userdata, void* args)
{
	JER_ARGS_PED_SKELETON* a = (JER_ARGS_PED_SKELETON*)args;
	WeaponBase* w = gCurrentWeapon;
	SVECTOR* vJPos = (SVECTOR*)a->jointPos;
	VECTOR* playerPos = (VECTOR*)a->playerPos;
	VECTOR* cameraPos = (VECTOR*)a->cameraPos;
	int handLimb = WL_RHAND;

	(void)userdata;

	if (w == NULL)
		return JER_RESULT_CONTINUE;

	/* weapons are on-foot only */
	if (player[0].playerCarId >= 0)
		return JER_RESULT_CONTINUE;

	/* the arm that holds the weapon follows the camera shoulder */
	handLimb = (w->shoulderSide > 0) ? WL_LHAND : WL_RHAND;

	if (a->phase == 0)
	{
		/* force the arm into a holding pose: override the accumulated
		 * offsets of the upper arm / forearm / hand bones so the arm
		 * extends forward. Values are in the ped's local frame. */
		if (gAiming)
			w->poseArmAim(a->skel);		/* presenting: raised, extended */
		else
			w->poseArm(a->skel);		/* hip carry */
	}
	else if (a->phase == 1 && !a->shadow)
	{
		/* world-space hand position (muzzle origin) */
		gHandPos[0] = vJPos[handLimb].vx + playerPos->vx;
		gHandPos[1] = vJPos[handLimb].vy + playerPos->vy;
		gHandPos[2] = vJPos[handLimb].vz + playerPos->vz;

		/* draw the weapon mesh at the hand, camera-relative, barrel along
		 * the view axis (identity matrix -> camera-aligned) */
		if (w->meshModel != NULL)
		{
			VECTOR pos;
			MATRIX ident;

			pos.vx = vJPos[handLimb].vx + playerPos->vx - cameraPos->vx;
			pos.vy = vJPos[handLimb].vy + playerPos->vy - cameraPos->vy;
			pos.vz = vJPos[handLimb].vz + playerPos->vz - cameraPos->vz;

			SetRotMatrix(&ident);
			ident.t[0] = 0;
			ident.t[1] = 0;
			ident.t[2] = 0;

			RenderModel(w->meshModel, &ident, &pos, 1, PLOT_NO_SHADE, 0, 0);

			/* restore the GTE translation the engine's own bone draws rely
			 * on ({0,0,0} set at the top of newShowTanner) — otherwise
			 * Tanner's body would be drawn offset by the hand position */
			{
				VECTOR zero = { 0, 0, 0 };

				gte_SetTransVector(&zero);
			}
		}
	}

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_FRAME — weapon update + laser line + impact marker        */
/* ================================================================== */

static int D2plOnFrame(void* userdata, void* args)
{
	PLAYER* lp = &player[0];
	int pad = 0;
	int padNew = 0;

	(void)userdata;
	(void)args;

	if (lp->padid >= 0)
	{
		pad = Pads[lp->padid].mapped;
		padNew = Pads[lp->padid].mapnew;
	}

	/* weapons only work on foot */
	if (lp->playerCarId >= 0)
	{
		gAiming = 0;
		return JER_RESULT_CONTINUE;
	}

	/* L1 = aim (only with a weapon equipped) */
	gAiming = (gCurrentWeapon != NULL && (pad & MPAD_L1) != 0);

#ifndef PSX
	/* laser-sight line from Tanner's hand to the evaluated aim point while
	 * aiming (flushed by the engine's DrawDebugOverlays each frame). A
	 * player aid — color from the settings, default red. */
	if (gAiming && gS.laserColor != D2PL_LASER_OFF && gCurrentWeapon != NULL &&
		(gHandPos[0] != 0 || gHandPos[1] != 0 || gHandPos[2] != 0))
	{
		VECTOR from;
		VECTOR to;
		CVECTOR col;

		D2plLaserColor(&col);

		from.vx = gHandPos[0];
		from.vy = gHandPos[1];
		from.vz = gHandPos[2];

		to.vx = gAimPoint[0];
		to.vy = gAimPoint[1];
		to.vz = gAimPoint[2];

		Debug_AddLine(from, to, col);
	}

	/* impact marker: a big black X at the last shot's aim point, visible
	 * for MARKER_LIFETIME frames so the player can trace where the weapon
	 * is hitting */
	if (gMarkerTimer > 0)
	{
		VECTOR c;
		CVECTOR black = { 0, 0, 0 };
		int dx, dz;
		int size;
		int dist;

		c.vx = gMarkerPos[0];
		c.vy = gMarkerPos[1];
		c.vz = gMarkerPos[2];

		/* size scales with the distance from the camera */
		dist = ABS(gMarkerPos[0] - camera_position.vx)
			+ ABS(gMarkerPos[1] - camera_position.vy)
			+ ABS(gMarkerPos[2] - camera_position.vz);

		size = jer_clamp_int(dist / 8, MARKER_MIN_SIZE, MARKER_MAX_SIZE);

		dx = size;
		dz = size;

		/* an asterisk: both diagonals + horizontal + vertical */
		{
			VECTOR p;

			p.vx = c.vx - dx; p.vy = c.vy - dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx + dx; p.vy = c.vy - dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx - dx; p.vy = c.vy + dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx + dx; p.vy = c.vy + dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx - dx; p.vy = c.vy; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx + dx; p.vy = c.vy; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx; p.vy = c.vy - dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);

			p.vx = c.vx; p.vy = c.vy + dz; p.vz = c.vz;
			Debug_AddLine(p, c, black);
		}

		gMarkerTimer--;
	}
#endif

	if (gCurrentWeapon != NULL)
	{
		/* lazily resolve the weapon mesh once models are loaded */
		if (gCurrentWeapon->meshModel == NULL)
			gCurrentWeapon->init();

		gCurrentWeapon->aiming = gAiming;

		/* clear the fired flag the weapon may have set last frame */
		gCurrentWeapon->fired = 0;

		gCurrentWeapon->update(pad, padNew);

		/* a round was fired this frame: stamp the impact marker */
		if (gCurrentWeapon->fired)
		{
			gMarkerTimer = MARKER_LIFETIME;
			gMarkerPos[0] = gAimPoint[0];
			gMarkerPos[1] = gAimPoint[1];
			gMarkerPos[2] = gAimPoint[2];
		}
	}

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_DRAW_OVERLAY — HUD                                        */
/* ================================================================== */

static int D2plOnOverlay(void* userdata, void* args)
{
	WeaponBase* w = gCurrentWeapon;
	char text[64];

	(void)userdata;
	(void)args;

	if (w == NULL)
		return JER_RESULT_CONTINUE;

	/* HUD only while on foot */
	if (player[0].playerCarId >= 0)
		return JER_RESULT_CONTINUE;

	/* weapon name + ammo bottom-left */
	sprintf(text, "%s  [%d/%d]", w->name, w->clip, w->reserve);
	SetTextColour(255, 255, 255);
	PrintString(text, 20, 220);

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* JER_EVENT_PAUSE_MENU — D2PL Settings bridge                         */
/* ================================================================== */

static const char* gLaserNames[D2PL_LASER_COUNT] = {
	"Off", "Red", "Green", "Blue", "White"
};

static char gD2plLabelBuf[D2PL_ITEM_COUNT][40];

static const char* D2plItemLabel(int item)
{
	switch (item)
	{
	case D2PL_ITEM_SENS_X:
		sprintf(gD2plLabelBuf[item], "Look Sens X: %d", gS.sensX);
		break;
	case D2PL_ITEM_SENS_Y:
		sprintf(gD2plLabelBuf[item], "Look Sens Y: %d", gS.sensY);
		break;
	case D2PL_ITEM_FOOT_DIST:
		sprintf(gD2plLabelBuf[item], "Foot Distance: %d", gS.footDist);
		break;
	case D2PL_ITEM_FOOT_LAT:
		sprintf(gD2plLabelBuf[item], "Foot Offset: %d", gS.footLat);
		break;
	case D2PL_ITEM_CAR_DIST:
		sprintf(gD2plLabelBuf[item], "Car Distance: %d", gS.carDist);
		break;
	case D2PL_ITEM_CAR_LAT:
		sprintf(gD2plLabelBuf[item], "Car Offset: %d%%", gS.carLatPct);
		break;
	case D2PL_ITEM_SHOULDER:
		sprintf(gD2plLabelBuf[item], "Shoulder: %s", gS.shoulder > 0 ? "Right" : "Left");
		break;
	case D2PL_ITEM_INVERT_H:
		sprintf(gD2plLabelBuf[item], "Invert Look X: %s", gS.invertH ? "ON" : "OFF");
		break;
	case D2PL_ITEM_INVERT_V:
		sprintf(gD2plLabelBuf[item], "Invert Look Y: %s", gS.invertV ? "ON" : "OFF");
		break;
	case D2PL_ITEM_FOV:
		sprintf(gD2plLabelBuf[item], "Base FOV: %d deg %s", gS.fovDeg,
			gS.fovEnabled ? "(ON)" : "(OFF)");
		break;
	case D2PL_ITEM_LASER:
		sprintf(gD2plLabelBuf[item], "Laser Sight: %s",
			gLaserNames[gS.laserColor]);
		break;
	default:
		gD2plLabelBuf[item][0] = 0;
		break;
	}

	return gD2plLabelBuf[item];
}

static void D2plAdjustItem(int item, int direction)
{
	switch (item)
	{
	case D2PL_ITEM_SENS_X:
		gS.sensX = jer_clamp_int(gS.sensX + direction * 4, 16, 160);
		break;
	case D2PL_ITEM_SENS_Y:
		gS.sensY = jer_clamp_int(gS.sensY + direction * 4, 16, 160);
		break;
	case D2PL_ITEM_FOOT_DIST:
		gS.footDist = jer_clamp_int(gS.footDist + direction * 20, 120, 900);
		break;
	case D2PL_ITEM_FOOT_LAT:
		gS.footLat = jer_clamp_int(gS.footLat + direction * 10, 0, 500);
		break;
	case D2PL_ITEM_CAR_DIST:
		gS.carDist = jer_clamp_int(gS.carDist + direction * 20, 100, 1000);
		break;
	case D2PL_ITEM_CAR_LAT:
		gS.carLatPct = jer_clamp_int(gS.carLatPct + direction * 2, 5, 60);
		break;
	case D2PL_ITEM_SHOULDER:
		if (direction != 0)
			gS.shoulder = (direction > 0) ? 1 : -1;
		break;
	case D2PL_ITEM_INVERT_H:
		if (direction != 0)
			gS.invertH = !gS.invertH;
		break;
	case D2PL_ITEM_INVERT_V:
		if (direction != 0)
			gS.invertV = !gS.invertV;
		break;
	case D2PL_ITEM_FOV:
		gS.fovDeg = jer_clamp_int(gS.fovDeg + direction * 1, 55, 90);
		if (direction != 0)
			gS.fovEnabled = 1;
		break;
	case D2PL_ITEM_LASER:
		if (direction != 0)
			gS.laserColor = (gS.laserColor + (direction > 0 ? 1 : D2PL_LASER_COUNT - 1))
				% D2PL_LASER_COUNT;
		break;
	default:
		break;
	}

	D2plSaveSettings();
}

static int D2plOnPauseMenu(void* userdata, void* args)
{
	JER_ARGS_PAUSE_MENU* a = (JER_ARGS_PAUSE_MENU*)args;

	(void)userdata;

	switch (a->action)
	{
		case JER_PAUSE_D2PL_GET_LABEL:
			if (a->value >= 0 && a->value < D2PL_ITEM_COUNT)
				a->result = (void*)D2plItemLabel(a->value);
			break;
		case JER_PAUSE_D2PL_ADJUST:
			if (a->value >= 0 && a->value < D2PL_ITEM_COUNT)
				D2plAdjustItem(a->value, (int)(intptr_t)a->result);
			break;
		default:
			break;
	}

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* Level start: reset                                                  */
/* ================================================================== */

static void D2plReset(void)
{
	int i;

	gLookIdle = 0;
	gLookPitch = 0;
	gLookPitchTarget = 0;
	gPedWasMoving = 0;

	gHandPos[0] = gHandPos[1] = gHandPos[2] = 0;
	gAimPoint[0] = gAimPoint[1] = gAimPoint[2] = 0;
	gAiming = 0;
	gMarkerTimer = 0;

	for (i = 0; i < gWeaponCount; i++)
	{
		gWeapons[i]->clip = gWeapons[i]->clipSize;
		gWeapons[i]->reserve = gWeapons[i]->maxReserve;
		gWeapons[i]->reloading = 0;
		gWeapons[i]->cooldown = 0;
		gWeapons[i]->fired = 0;
	}
}

static int D2plOnGameStart(void* userdata, void* args)
{
	(void)userdata;
	(void)args;

	D2plReset();

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* Demo weapon: the frontend-sound pistol                              */
/* ================================================================== */

/*
 * DemoPistol — proves the template end to end. It reuses the frontend menu
 * sounds via the base class defaults: primary fire = menu ACCEPT (FESound
 * 2), secondary fire = menu BACKOUT (FESound 0). No game weapon mesh exists
 * in the data files, so it holds the "BOMB" model as a placeholder until a
 * real pistol model is added — just point meshName at it.
 */
class DemoPistol : public WeaponBase
{
public:
	DemoPistol()
	{
		id = "pistol";
		name = "9mm Pistol";
		damage = 100;
		fireMode = 0;		/* semi-auto */
		clipSize = 12;
		maxReserve = 60;
		fireRate = 6;
		reloadTime = 45;
		range = 30000;
		spread = 0;
		meshName = "BOMB";	/* placeholder mesh */
		shoulderSide = -1;
		aimZoom = 0;
	}
};

static DemoPistol gDemoPistol;

JER_MODULE_ENTRY(jer_module_d2pl_entry)(JERICHO_CONTEXT* ctx)
{
	ctx->jer_register_module(ctx,
		"d2pl",			/* id */
		"Driver 2 Parallel Lines",	/* name */
		"1.0.0",		/* version */
		"DeepSeek",		/* author */
		"Modern dual-stick camera (free-look orbit, GTA-style framing, FOV) + overridable weapon system with aim/fire/ammo/laser.",	/* description */
		"",			/* dependencies */
		JERICHO_SDK_VERSION);	/* SDK this module was built against */

	/* settings from mods/config/d2pl.ini; write them back so the file
	 * exists on first boot (hand-editable tuning surface) */
	D2plLoadSettings();
	D2plSaveSettings();

	/* build the demo pistol and register it */
	gDemoPistol.init();
	WeaponSystem_Register(&gDemoPistol);
	WeaponSystem_Equip(&gDemoPistol);

	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, D2plOnLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, D2plOnCamera, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, D2plOnPedInput, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_SKELETON, D2plOnSkeleton, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_FRAME, D2plOnFrame, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, D2plOnOverlay, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, D2plOnPauseMenu, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, D2plOnGameStart, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_INIT, D2plOnGameStart, NULL, 0);

	ctx->jer_log(ctx, "[d2pl] Driver 2 Parallel Lines ready (%d weapon(s): %s; sens %d/%d, foot %d/%d, car %d/%d%%, shoulder %s, fov %d(%s), laser %s)\n",
		gWeaponCount,
		gCurrentWeapon != NULL ? gCurrentWeapon->id : "none",
		gS.sensX, gS.sensY,
		gS.footDist, gS.footLat,
		gS.carDist, gS.carLatPct,
		gS.shoulder > 0 ? "right" : "left",
		gS.fovDeg, gS.fovEnabled ? "on" : "off",
		gLaserNames[gS.laserColor]);
}
