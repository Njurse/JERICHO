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
#include "pedest.h"		/* ActivatePlayerPedestrian (-onfoot harness) */
#include "civ_ai.h"		/* PingOutCar (-onfoot harness) */
#include "dr2roads.h"	/* MapHeight — terrain height for the ground cap */
#include "PsyX/PsyX_public.h"	/* PsyX_GetScreenSize (FOV math) */

#include <math.h>

/* the debug line API used by the engine's collision overlays (PC only) */
#ifndef PSX
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
/* JERICHO-HOOK (DebugOverlay.cpp): depth-sorted variant — the line joins
 * the world's ordering table at its Z bucket, so walls occlude it */
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

/* ================================================================== */
/* Settings (persisted to mods/config/d2pl.ini)                       */
/* ================================================================== */

typedef struct D2PL_SETTINGS
{
	int sensX;		/* view-change sensitivity, horizontal (64 = default) */
	int sensY;		/* view-change sensitivity, vertical (64 = default) */
	int joyCamera;		/* 1 = right-stick orbit/look control (default on) */
	int footDist;		/* on-foot camera pull-in distance */
	int footLat;		/* on-foot shoulder offset */
	int footHeight;		/* on-foot camera height (inverted y: below the body) */
	int carDist;		/* in-car pull-in (fraction of the view axis) */
	int carLatPct;		/* in-car lateral, % of the car's bbox width */
	int shoulder;		/* -1 = left shoulder, +1 = right */
	int invertH;		/* 1 = invert horizontal look */
	int invertV;		/* 1 = invert vertical look */
	int fovEnabled;		/* 1 = override the engine FOV */
	int fovDeg;		/* target horizontal FOV (degrees) */
	int laserColor;		/* D2PL_LASER_* index (0 = off) */
	int cameraEnabled;	/* 1 = the mod's camera/movement run (kill-switch) */
} D2PL_SETTINGS;

#define D2PL_CFG_MOD "d2pl"
#define D2PL_CFG_VERSION 6	/* bump when defaults change — a stale profile is
				   deleted and regenerated from scratch */

static D2PL_SETTINGS gS = {
	58,			/* sensX — baseline sensitivity, ~10% under the 64
				   reference (the persisted config is regenerated) */
	58,			/* sensY */
	1,			/* joyCamera — right-stick look/orbit on by default */
	1040,			/* footDist — doubled: a high, wide GTA-like view that
				   can pitch down steeper and show more around the ped */
	120,			/* footLat (shoulder) */
	-300,			/* footHeight — the orbit moved up (camera ~300 units
				   above the waist anchor, still aiming at the player) */
	575,			/* carDist (+15% default distance) */
	28,			/* carLatPct */
	-1,			/* shoulder (left) */
	0,			/* invertH */
	0,			/* invertV */
	1,			/* fovEnabled */
	82,			/* fovDeg (+10 from the old 72 default) */
	D2PL_LASER_DEFAULT,	/* laserColor (red) */
	1			/* cameraEnabled */
};

static void D2plSaveSettings(void);	/* forward (load regenerates on stale) */

static void D2plLoadSettings(void)
{
	int storedVersion = jer_config_get_int(D2PL_CFG_MOD, "cfg_version", 1);

	/* a version bump means the old profile is stale: DELETE it and keep
	 * the new defaults; the save below regenerates a fresh file so no
	 * stale values (or unknown keys) survive */
	if (storedVersion < D2PL_CFG_VERSION)
	{
		jer_config_clear(D2PL_CFG_MOD);
		D2plSaveSettings();
	}
	else
	{
		gS.sensX = jer_config_get_int(D2PL_CFG_MOD, "sens_x", gS.sensX);
		gS.sensY = jer_config_get_int(D2PL_CFG_MOD, "sens_y", gS.sensY);
		gS.joyCamera = jer_config_get_int(D2PL_CFG_MOD, "joy_camera", gS.joyCamera);
		gS.footDist = jer_config_get_int(D2PL_CFG_MOD, "foot_dist", gS.footDist);
		gS.footLat = jer_config_get_int(D2PL_CFG_MOD, "foot_lat", gS.footLat);
		gS.footHeight = jer_config_get_int(D2PL_CFG_MOD, "foot_height", gS.footHeight);
		gS.carDist = jer_config_get_int(D2PL_CFG_MOD, "car_dist", gS.carDist);
		gS.carLatPct = jer_config_get_int(D2PL_CFG_MOD, "car_lat_pct", gS.carLatPct);
		gS.shoulder = jer_config_get_int(D2PL_CFG_MOD, "shoulder", gS.shoulder);
		gS.invertH = jer_config_get_int(D2PL_CFG_MOD, "invert_h", gS.invertH);
		gS.invertV = jer_config_get_int(D2PL_CFG_MOD, "invert_v", gS.invertV);
		gS.fovEnabled = jer_config_get_int(D2PL_CFG_MOD, "fov_enabled", gS.fovEnabled);
		gS.fovDeg = jer_config_get_int(D2PL_CFG_MOD, "fov_deg", gS.fovDeg);
		gS.laserColor = jer_config_get_int(D2PL_CFG_MOD, "laser_color", gS.laserColor);
		gS.cameraEnabled = jer_config_get_int(D2PL_CFG_MOD, "camera_enabled", gS.cameraEnabled);
	}

	/* clamp so a hand-edited file can't break the camera; values outside
	 * the ranges are reset to the defaults */
	gS.sensX = jer_clamp_int(gS.sensX, 16, 160);
	gS.sensY = jer_clamp_int(gS.sensY, 16, 160);
	gS.joyCamera = (gS.joyCamera != 0) ? 1 : 0;
	if (gS.footDist < 80 || gS.footDist > 1600)
		gS.footDist = 1040;
	if (gS.footLat < 0 || gS.footLat > 300)
		gS.footLat = 120;
	if (gS.footHeight < -500 || gS.footHeight > 0)
		gS.footHeight = -300;
	gS.carDist = jer_clamp_int(gS.carDist, 100, 1000);
	gS.carLatPct = jer_clamp_int(gS.carLatPct, 5, 60);
	gS.shoulder = (gS.shoulder > 0) ? 1 : -1;
	gS.fovDeg = jer_clamp_int(gS.fovDeg, 55, 90);
	gS.laserColor = (gS.laserColor < 0 || gS.laserColor >= D2PL_LASER_COUNT)
		? D2PL_LASER_DEFAULT : gS.laserColor;
}

static void D2plSaveSettings(void)
{
	jer_config_set_int(D2PL_CFG_MOD, "cfg_version", D2PL_CFG_VERSION);
	jer_config_set_int(D2PL_CFG_MOD, "sens_x", gS.sensX);
	jer_config_set_int(D2PL_CFG_MOD, "sens_y", gS.sensY);
	jer_config_set_int(D2PL_CFG_MOD, "joy_camera", gS.joyCamera);
	jer_config_set_int(D2PL_CFG_MOD, "foot_dist", gS.footDist);
	jer_config_set_int(D2PL_CFG_MOD, "foot_lat", gS.footLat);
	jer_config_set_int(D2PL_CFG_MOD, "foot_height", gS.footHeight);
	jer_config_set_int(D2PL_CFG_MOD, "car_dist", gS.carDist);
	jer_config_set_int(D2PL_CFG_MOD, "car_lat_pct", gS.carLatPct);
	jer_config_set_int(D2PL_CFG_MOD, "shoulder", gS.shoulder);
	jer_config_set_int(D2PL_CFG_MOD, "invert_h", gS.invertH);
	jer_config_set_int(D2PL_CFG_MOD, "invert_v", gS.invertV);
	jer_config_set_int(D2PL_CFG_MOD, "fov_enabled", gS.fovEnabled);
	jer_config_set_int(D2PL_CFG_MOD, "fov_deg", gS.fovDeg);
	jer_config_set_int(D2PL_CFG_MOD, "laser_color", gS.laserColor);
	jer_config_set_int(D2PL_CFG_MOD, "camera_enabled", gS.cameraEnabled);
}

/* ================================================================== */
/* Camera tuning constants                                             */
/* ================================================================== */

#define LOOK_DEADZONE 24	/* stick counts below this are "idle" */
#define LOOK_YAW_SIGN -1	/* -1 = push right orbits the camera right */

/* aim mode: the look speed is halved while aiming for finer control */
#define AIM_LOOK_DIV 2

/* the L2/R2 look buttons: the camera eases to the car's left/right side
 * view (90 degrees, car-relative) — the stock TurnHead head-rot path
 * breaks the module's transforms, so the module owns the buttons */
#define LOOK_SIDE_ANGLE 1024
#define LOOK_SIDE_DIV 4		/* ease divisor: ~0.1 s to reach the side view */

/* on-foot startup momentum: while the ped is just starting to move (speed
 * below this) the camera eases with a touch more lag, then tightens up */
#define FOOT_LAG_SPEED 6
#define FOOT_LAG_DIV 8
#define YAW_RATE 256		/* max orbit speed at full stick, sensX = 64
				   (x4: the default was too slow) */
#define YAW_RAMP 20		/* frames to reach full orbit speed (~1/3 s at
				   60 fps) — the yaw "gains momentum" instead of
				   jumping to its max immediately */
#define LOOK_PITCH_SIGN -1	/* -1 = pushing up RAISES the camera on its
				   orbit around the player (see the header note) */
#define PITCH_MAX 512		/* ~45 degrees of pitch up/down each way — the
				   vertical is deliberately tamer than the horizontal so it
				   doesn't feel janky at full deflection */
#define FOV_PITCH_SCALE 4	/* scr_z delta per 128 pitch units (GTA4-style:
				   looking down from above narrows the FOV, up widens) */
#define LOOK_SETTLE_FOOT 100	/* frames idle before settling back (on foot) */
#define LOOK_SETTLE_CAR 45	/* cars settle back after this long idle... */
#define LOOK_SETTLE_CAR_MOVING 8	/* ...but a car in motion resets almost
				   immediately: it interpolates back to its default
				   angle after a few frames of stick silence */
#define LOW_SPEED_GRACE 4	/* car wheel_speed at/below which the orbit stays
				   gripped (no camera swing at a standstill) */

#define MOVE_DEADZONE 24	/* left-stick deadzone */
#define MOVE_TURN_SIGN 1	/* +1 = positive angle delta turns right */
#define MOVE_STICK_SIGN_X 1	/* flip if your pad's X axis is inverted */
#define MOVE_STICK_SIGN_Y 1	/* flip if your pad's Y axis is inverted */
#define MOVE_LERP_DIV 8		/* responsive exponential heading lerp: 1/8 of the
				   remaining gap toward the stick-derived heading each
				   frame (fast when far, eases in) */
#define RUN_TURN_LIMIT 128	/* max turn per frame while running (~11.25 deg,
				   ~675 deg/s): well beyond the original 32-unit
				   restriction so Tanner decisively turns toward
				   the stick heading (even a 180) instead of
				   creeping around; the exponential lerp keeps it
				   smooth, the cap keeps it from snapping */
#define PIVOT_TURN_LIMIT 256	/* standstill pivot speed (~22.5 deg/frame):
				   near-instant so the player starts running the new
				   way immediately, GTA-style */

#define CAR_HEIGHT -105		/* in-car camera drop (more grounded) */
#define CAR_ORBIT_UP -160	/* raise the vehicle orbit/focal point (Driver 2
				   uses INVERTED y — up = -y); the camera gravitates
				   toward looking AHEAD of the car near the forward
				   angle so view-panning in isn't jarring */
#define CAR_LOOK_AHEAD 5000	/* the settled camera focuses this far AHEAD of
				   the car so you see where you're driving */

/* ground cap: Driver 2 uses INVERTED y (vy grows downward), so the road is
 * a heightfield the camera must not sink below. Mirror the engine's own
 * chase-cam clamp (camera.c: cammapht = carheight - MapHeight - 100) — the
 * camera stays at-or-above the terrain line and slides along the surface. */
#define TERRAIN_CLEAR_FOOT 15
#define TERRAIN_CLEAR_CAR 50
/* ground-slide "forced look-up": when the terrain cap pins the camera (it
 * would sink into the ground), the view tilts slightly upward — a fraction
 * of the would-be intersection depth, capped — so the camera reads as
 * being pushed up out of the ground instead of boring into it. */
#define GROUND_LOOKUP_SCALE 8	/* angle units per 256 units of penetration */
#define GROUND_LOOKUP_MAX 200	/* cap the forced up-tilt (~17.5 deg) */
#define FOOT_AIM_HEIGHT 90	/* on-foot aim point: the torso ~90 units above
				   the waist anchor (camera frame: up = -y) */
#define CAR_SPEED_DIV 2		/* car pull-in fades with speed (GTA4 zoom-out) */

/* natural settle-back (GTA IV State A): the module eases the orbit toward
 * the blended natural heading instead of the engine's fixed-rate settle */
#define SETTLE_DIV 12		/* 1/12 of the gap per frame — lazy, cinematic */
#define VEL_NOISE 16		/* base-delta noise floor before velocity counts */
#define VEL_BLEND_SPEED_SCALE 24	/* velocity-blend weight per speed unit */
#define VEL_BLEND_MAX 2900	/* cap the base blend weight (~0.7 of 4096);
				   the drift boost may push past this */
#define VEL_BLEND_ABS_MAX 3500	/* absolute ceiling (~0.85) — never full weight,
				   so the heading vector never fully cancels */
#define VEL_BLEND_SLIP_MIN 384	/* slip angle where the drift boost starts */
#define VEL_BLEND_SLIP_DIV 2	/* slip boost per unit of slip */
#define FOV_SPEED_SCALE 512	/* subtle FOV widening with smoothed speed */
#define HEIGHT_SPEED_SCALE 256	/* subtle camera height rise with speed */
#define FOOT_WALK_SPEED 14	/* on-foot speed at which the camera sits at its
				   base distance (pull in when slower, extend when
				   running — relative-framing doc) */
#define FOOT_SPEED_DIST 4	/* distance units added per unit of run speed */
#define MAX_POS_STEP 600	/* the camera position NEVER snaps during play: a
				   big correction travels at a bounded rate — the
				   writeup's 'never snap, always blend' applies to the
				   position rig as well as the rotation */
#define CONTEXT_JUMP 30000	/* a target jump bigger than this is a context
				   switch (level load, intro sweep -> play, spawn):
				   re-place instantly instead of flying across the
				   map at MAX_POS_STEP */

/* Instability Governor (spin-out recovery, GTA IV §4): when the car's
 * angular rate spikes (spin, crash), the camera stops trying to be clever
 * and holds its orientation until the car settles. Hysteresis: trigger
 * above the upper threshold, release only after the rate has stayed below
 * the lower threshold for a confirmation window. Priority below Manual
 * Look, above Natural. */
#define INSTAB_TRIGGER 192	/* smoothed deg-ish units/frame that claims focus */
#define INSTAB_RELEASE 64	/* must drop below this to release */
#define INSTAB_CONFIRM 20	/* frames of calm before releasing */

/* aim camera (on foot) */
#define AIM_PULL 2000		/* /4096: pull toward Tanner while aiming */
#define AIM_HEIGHT -2
#define AIM_ZOOM 160		/* scr_z increase -> FOV decrease (focus) */
#define AIM_LATERAL 25		/* shoulder bias while aiming (ped scale ~130) */

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
static int gYawSpeed;		/* current orbit speed (momentum: ramps toward
				   the stick's max over ~1/3 s, decays on release) */
static int gGripDist;		/* orbit radius frozen for the whole pan (the
				   spherical-pan grip) — hoisted so level resets can
				   clear it */
static int gMoveRefHeading;	/* camera heading frozen at view-manipulation start
				   (the on-foot movement reference frame, GTA IV §8) */
static int gMoveRefActive;	/* 1 while that freeze is in effect */
static int gVelAngle;		/* smoothed ground-velocity heading (0..4095) */
static int gSpeedSmooth;	/* smoothed wheel speed (FIXEDH units, 0 on foot) */
static int gBobLastX;	/* last on-foot base position (view bob motion) */
static int gBobLastZ;
static int gLastBaseX, gLastBaseZ;
static int gCamInitialized;	/* one-shot: snap the spawn orbit angle behind */
static int gSettleInfluence;	/* natural-settle influence fade (0..4096): ramps
				   in on release so the ease eases IN — no correction
				   snap at the handoff, live target throughout */
static int gAngRate;		/* smoothed car-body angular rate (units/frame) */
static int gLastCarDir;		/* previous car heading for the rate delta */
static int gInstability;	/* 1 = the Instability Governor holds the camera */
static int gInstabConfirm;	/* frames the rate has stayed calm (hysteresis) */
static int gFootSpeedSmooth;	/* smoothed on-foot speed (pPed->speed) */
static int gLastTannerPad;	/* previous frame's on-foot pad — the hook carries
				   no padNew, so the X-reload edge is tracked here */
static int gTrackedCarId = -2;	/* the car the governor is sampling (seeds the
				   angular-rate delta on car entry, so the first
				   in-car frame can't false-trigger a spin) */
static VECTOR gCamSmooth;	/* smoothed camera position (live target + bounded
				   rate; hoisted so level starts re-place it) */
static int gCamSmoothValid;

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
static int gPedScaleLogged;	/* one-shot per-level ped-scale diagnostic */

/* mouse integration: relative motion consumed by the look handler (yaw/pitch
 * nudges on top of the stick); buttons map to aim (right), primary fire
 * (left), secondary fire (middle). Per-pixel nudge at sens 64:
 * (dx * sens * MOUSE_YAW) / (64*128). */
#include <SDL.h>

#define MOUSE_YAW	1152
#define MOUSE_PITCH	1152

/* input-device authority: the mouse only drives the view while it is the
 * recent input device (motion/click grants ~0.75 s; live controller
 * analog revokes it immediately) — a connected controller + an idle mouse
 * must never fight over the camera */
#define MOUSE_AUTHORITY_FRAMES 45

static int gMouseDX = 0;	/* relative mouse motion this frame (look) */
static int gMouseDY = 0;
static Uint32 gLastMouseButtons = 0;	/* for synthesized press edges */
static int gMouseActive = 0;	/* frames of remaining mouse authority */
static int gLookBackHeld = 0;	/* L2+R2/L3 look-back was held last frame (a
				   release must swing back even at standstill) */

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

/* the natural heading the settled camera tracks behind: for a car it's
 * the speed-weighted blend of the body heading and the ground-velocity
 * heading; on foot it's the ped's facing. Computed FRESH every call from
 * live state — never a snapshot — so resuming automatic tracking after a
 * pause (manual look, spin-out) never corrects against a stale target.
 *
 * The blend weights the direction VECTORS (RSIN/RCOS components), not
 * the angles directly, so a heading crossing 0/360 cannot snap. */
static int D2plNaturalHeading(PLAYER* lp, int inCar)
{
	int natural = inCar ? car_data[lp->playerCarId].hd.direction : lp->dir;

	if (inCar)
	{
		int w = jer_clamp_int(gSpeedSmooth * VEL_BLEND_SPEED_SCALE, 0, VEL_BLEND_MAX);
		int slip = ABS(DIFF_ANGLES(natural, gVelAngle));

		if (slip > VEL_BLEND_SLIP_MIN)
			w += (slip - VEL_BLEND_SLIP_MIN) / VEL_BLEND_SLIP_DIV;

		/* cap the TOTAL so the boost can push past the base cap but never
		 * reach full velocity weight */
		if (w > VEL_BLEND_ABS_MAX)
			w = VEL_BLEND_ABS_MAX;

		{
			int bx = FIXEDH(RSIN(natural) * (4096 - w) + RSIN(gVelAngle) * w);
			int bz = FIXEDH(RCOS(natural) * (4096 - w) + RCOS(gVelAngle) * w);

			if (ABS(bx) + ABS(bz) > 16)
				natural = ratan2(bx, bz);
		}
	}

	return natural;
}

static int D2plOnLook(void* userdata, void* args)
{
	JER_ARGS_CAMERA_LOOK* a = (JER_ARGS_CAMERA_LOOK*)args;
	PLAYER* lp = (PLAYER*)a->player;
	int inCar = (lp->playerCarId >= 0);
	int stickX = a->stickX;
	int stickY = a->stickY;
	int usingStick;
	int stockLook;

	if (gS.cameraEnabled == 0)
		return JER_RESULT_CONTINUE;

	(void)userdata;

	if (lp->padid < 0)
		return JER_RESULT_CONTINUE;

	/* smoothed speed + ground-velocity heading (GTA IV §2/§5): the base
	 * delta feeds the velocity angle; the wheel speed feeds the smoothed
	 * speed used by the natural blend, the pull and the FOV/height */
	{
		int dx = lp->pos[0] - gLastBaseX;
		int dz = lp->pos[2] - gLastBaseZ;
		long long d2 = (long long)dx * dx + (long long)dz * dz;
		int dist = (d2 > 0x7fffffffLL) ? 32767 : SquareRoot0((int)d2);

		gLastBaseX = lp->pos[0];
		gLastBaseZ = lp->pos[2];

		if (dist > VEL_NOISE)
			gVelAngle = (gVelAngle + DIFF_ANGLES(gVelAngle, ratan2(dx, dz)) / 6) & 0xfff;

		gSpeedSmooth += ((inCar ? D2plCarSpeed(lp) : 0) - gSpeedSmooth) / 8;

		if (!inCar && lp->pPed != NULL)
			gFootSpeedSmooth += (ABS(lp->pPed->speed) - gFootSpeedSmooth) / 8;
		else
			gFootSpeedSmooth = 0;
	}

	/* Instability Governor tracking: smoothed car-body angular rate with
	 * hysteresis (trigger high, release low after a confirmation window) */
	if (inCar)
	{
		int dir = car_data[lp->playerCarId].hd.direction;

		if (gTrackedCarId != lp->playerCarId)
		{
			/* just entered this car: seed the delta so the first sample
			 * can't read a huge fake rate from an uninitialized last */
			gTrackedCarId = lp->playerCarId;
			gLastCarDir = dir;
			gAngRate = 0;
		}
		else
		{
			gAngRate += (ABS(DIFF_ANGLES(gLastCarDir, dir)) - gAngRate) / 4;
			gLastCarDir = dir;
		}

		if (gInstability == 0 && gAngRate > INSTAB_TRIGGER)
		{
			gInstability = 1;
			gInstabConfirm = 0;
		}
		else if (gInstability != 0)
		{
			if (gAngRate <= INSTAB_RELEASE)
			{
				gInstabConfirm++;

				if (gInstabConfirm >= INSTAB_CONFIRM)
					gInstability = 0;
			}
			else
			{
				gInstabConfirm = 0;
			}
		}
	}
	else
	{
		gAngRate = 0;
		gInstability = 0;
		gInstabConfirm = 0;
		gTrackedCarId = -2;
	}

	/* one-shot at spawn: the engine starts the orbit 45 degrees off
	 * (direction + 1536, players.c) — snap it straight behind the
	 * player/car so the default angle isn't off to the side */
	if (!gCamInitialized)
	{
		int facing = inCar ? car_data[lp->playerCarId].hd.direction : lp->dir;

		gCamInitialized = 1;
		lp->cameraAngle = (facing + gCameraAngle) & 0xfff;
	}

	/* while aiming the L2/R2 stock look buttons must not fire */
	if (gAiming)
		a->suppress = 1;

	usingStick = gS.joyCamera &&
		(ABS(stickX) > LOOK_DEADZONE || ABS(stickY) > LOOK_DEADZONE ||
		 (gMouseActive > 0 && (gMouseDX != 0 || gMouseDY != 0)));
	stockLook = (a->paddCamera & (CAMERA_PAD_LOOK_LEFT | CAMERA_PAD_LOOK_RIGHT |
		CAMERA_PAD_LOOK_BACK | CAMERA_PAD_LOOK_BACK_DED)) != 0;

	if (usingStick)
	{
		gSettleInfluence = 0;	/* a new grip aborts the fade: fresh start */

		if (gLookIdle > 0 || gGripDist == 0)
			gGripDist = lp->cameraDist;	/* grip just started: freeze now */

		lp->cameraDist = gGripDist;	/* hold the radius every frame */

		/* view manipulation is live: freeze the on-foot movement
		 * reference frame to the camera heading at grip start (GTA IV §8) */
		if (gLookIdle > 0 || gMoveRefActive == 0)
		{
			int rfx = lp->pos[0] - camera_position.vx;
			int rfz = lp->pos[2] - camera_position.vz;

			gMoveRefHeading = (rfx == 0 && rfz == 0) ? lp->dir : ratan2(rfx, rfz);
		}

		gMoveRefActive = 1;

		{
			int yawMax = (YAW_RATE * gS.sensX) / 64;
			int pitchMax = (PITCH_MAX * gS.sensY) / 64;
			int sX = gS.invertH ? -stickX : stickX;
			int sY = gS.invertV ? -stickY : stickY;
			int aimDiv = gAiming ? AIM_LOOK_DIV : 1;	/* slower while aiming */
			int targetSpeed;

			/* orbit: the angle moves by a momentum-driven speed. The speed
			 * ramps from 0 up to the stick's max (proportional to deflection)
			 * over ~1/3 of a second (YAW_RAMP frames) — the view "gains
			 * momentum" instead of jumping to full speed. The orbit stays
			 * gripped so the engine's settle-back lerp can't fight it;
			 * vertical is clamped below. */
			gLookIdle = 0;
			a->suppress = 1;
		a->gripOrbit = 1;

		targetSpeed = (LOOK_YAW_SIGN * sX * yawMax) / (127 * aimDiv);

		/* ramp with a minimum step of 1 so tiny deflections still move */
		{
			int step = (targetSpeed - gYawSpeed) / YAW_RAMP;

			if (step == 0 && targetSpeed != gYawSpeed)
				step = (targetSpeed > gYawSpeed) ? 1 : -1;

			gYawSpeed += step;
		}

		lp->cameraAngle = (lp->cameraAngle + gYawSpeed) & 0xfff;

		/* clamp so the camera can never swing past straight up/down even at
		 * the highest sensitivity setting */
		pitchMax = jer_clamp_int(pitchMax, 0, 900);

		gLookPitchTarget = jer_clamp_int(
			(LOOK_PITCH_SIGN * sY * pitchMax) / (128 * aimDiv), -pitchMax, pitchMax);

		/* mouse motion nudges the orbit/pitch on top of the stick — direct
		 * (not momentum-ramped): a flick is a flick, smooth but immediate.
		 * NOTE: the mouse YAW sign is deliberately the opposite of the
		 * stick's (-LOOK_YAW_SIGN) — the user's live test showed the
		 * mouse horizontal was mirrored. Don't "fix" it back. */
		{
			/* clamp the per-frame delta so a warp/flick spike can't overflow
			 * the fixed-point products below */
			int mX = jer_clamp_int(gS.invertH ? -gMouseDX : gMouseDX, -512, 512);
			int mY = jer_clamp_int(gS.invertV ? -gMouseDY : gMouseDY, -512, 512);

			lp->cameraAngle = (lp->cameraAngle +
				(-LOOK_YAW_SIGN * mX * gS.sensX * MOUSE_YAW) / (64 * 128 * aimDiv)) & 0xfff;

			gLookPitchTarget = jer_clamp_int(gLookPitchTarget +
				(LOOK_PITCH_SIGN * mY * gS.sensY * MOUSE_PITCH) / (64 * 128 * aimDiv),
				-pitchMax, pitchMax);
		}

		/* consumed (or ignored while joystick look is off) — never let the
		 * deltas linger into the next frame */
		gMouseDX = 0;
		gMouseDY = 0;
		}
	}
	else if (stockLook && gAiming == 0)
	{
		/* the module OWNS the L2/R2/L3 look buttons — the stock TurnHead
		 * head-rot/headTarget path fights the module's transforms, so
		 * instead of yielding we orbit the camera itself: quickly to the
		 * left/right side view (L2/R2), instantly 180 degrees to look
		 * behind (L3 or L2+R2 together). The suppress masks the LOOK bits
		 * so the stock branches never run; gripOrbit stops the engine's
		 * settle-back lerp from fighting the swing. */
		int lookBack = (a->paddCamera & CAMERA_PAD_LOOK_BACK) == CAMERA_PAD_LOOK_BACK
			|| (a->paddCamera & CAMERA_PAD_LOOK_BACK_DED) != 0;

		gLookIdle = 0;
		gYawSpeed = 0;
		a->suppress = 1;
		a->gripOrbit = 1;
		gLookBackHeld = lookBack;

		if (lookBack)
		{
			/* the classic rear view, IDEMPOTENT while held: the camera sits
			 * in front of the car looking back (the engine's own look-back
			 * uses the same camAngle = baseDir) — an absolute target, so a
			 * held L2+R2/L3 holds the view instead of toggling 180 degrees
			 * every frame */
			lp->cameraAngle = D2plNaturalHeading(lp, inCar);
		}
		else
		{
			/* quickly ease to the car-relative side view (absolute target,
			 * not cumulative, so a held button lands once and stays) */
			int natural = D2plNaturalHeading(lp, inCar);
			int side = (a->paddCamera & CAMERA_PAD_LOOK_LEFT) != 0 ? -1 : 1;
			int target = (natural + gCameraAngle + side * LOOK_SIDE_ANGLE) & 0xfff;

			lp->cameraAngle = jer_lerp_angle(lp->cameraAngle, target, LOOK_SIDE_DIV);
		}
	}
	else
	{
		/* stick released: the momentum decays quickly so the settle-back
		 * doesn't jerk; a vehicle in motion resets almost immediately */
		gYawSpeed -= gYawSpeed / 8;

		if (ABS(gYawSpeed) < 8)
			gYawSpeed = 0;	/* kill the residual truncation drift */

		gLookIdle++;

		{
			int settleFrames = inCar
				? (D2plCarSpeed(lp) > LOW_SPEED_GRACE
					? LOOK_SETTLE_CAR_MOVING : LOOK_SETTLE_CAR)
				: LOOK_SETTLE_FOOT;

			if (gLookIdle > settleFrames)
		{
			/* released: the module's NATURAL settle takes over (GTA IV
			 * §1 State A) — ease the orbit toward the live blended natural
			 * heading via an influence fade. EXCEPT while the Instability
			 * Governor holds (the car is spinning/crashing: the camera
			 * stabilizes instead of trying to be clever) and at a
			 * standstill in a car (reverse grace: no auto swing unless the
			 * player is deliberately moving backward). */
			if (gInstability)
			{
				a->suppress = 1;
				a->gripOrbit = 1;
				gSettleInfluence = 0;	/* fresh fade when stability returns */
			}
			else if (inCar && D2plCarSpeed(lp) <= LOW_SPEED_GRACE && gLookBackHeld == 0)
			{
				/* reverse grace: hold at standstill — EXCEPT right after a
				 * look-back release, which must swing back out of the
				 * rear view (gLookBackHeld is cleared by the settle below) */
				a->suppress = 1;
				a->gripOrbit = 1;
			}
			else
			{
				int natural = D2plNaturalHeading(lp, inCar);
				int delta;

				gLookBackHeld = 0;	/* the rear view is released: settle */

				/* influence fade (the shared underlying trick): ramp how
				 * much of the live target is applied from 0 -> 1 instead of
				 * starting a fresh point-to-point blend — the target kept
				 * computing during the pan, so there is no correction snap */
				gSettleInfluence += (4096 - gSettleInfluence) / 16;

				delta = (DIFF_ANGLES(lp->cameraAngle, (natural + gCameraAngle) & 0xfff)
					* gSettleInfluence) / SETTLE_DIV / 4096;

				lp->cameraAngle = (lp->cameraAngle + delta) & 0xfff;

				a->suppress = 1;
				a->gripOrbit = 1;	/* the module owns the settle now */
				gLookPitchTarget = 0;
				gYawSpeed = 0;	/* kill the stale momentum so re-gripping
							   the stick doesn't jump */
				gMoveRefActive = 0;	/* the settle has begun: re-sync the
							   movement reference to the live camera */
			}
		}
		else
		{
			/* still inside the hold window: keep the orbit + pitch */
			a->suppress = 1;
			a->gripOrbit = 1;
		}
	}
	}

	/* smooth the pitch toward its target (yaw is applied directly) */
	gLookPitch = jer_lerp_int(gLookPitch, gLookPitchTarget, 6);

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

	if (gS.cameraEnabled == 0)
		return JER_RESULT_CONTINUE;

	(void)userdata;

	if (lp->padid < 0 || lp->pPed == NULL)
		return JER_RESULT_CONTINUE;

	/* on foot, the CROSS (X) button RELOADS the weapon instead of walking
	 * forward (stock: X = run). The press edge is tracked locally since
	 * the hook carries no padNew; D-pad up still walks forward. */
	if (lp->playerCarId < 0)
	{
		if ((a->pad & MPAD_CROSS) != 0 && (gLastTannerPad & MPAD_CROSS) == 0 &&
			gCurrentWeapon != NULL)
		{
			gCurrentWeapon->reload();
		}

		gLastTannerPad = a->pad;
		a->pad &= ~MPAD_CROSS;
	}
	else
	{
		gLastTannerPad = 0;	/* in a car: clear the edge tracker so the
					   first on-foot frame reads a clean X press */
	}

	mag = ABS(stickX) + ABS(stickY);

	if (mag <= MOVE_DEADZONE)
	{
		gPedWasMoving = 0;
		return JER_RESULT_CONTINUE;	/* stock handling */
	}

	/* where the camera looks, as a world heading (movement convention:
	 * heading h moves along (RSIN h, RCOS h)) — direction from the camera
	 * to Tanner, the same vector the chase cam aims along. While the
	 * player is actively view-manipulating (right stick), the reference
	 * frame is FROZEN to the camera heading at manipulation start so
	 * panning the camera can't bend the character's path (GTA IV §8). */
	if (gMoveRefActive)
	{
		camHeading = gMoveRefHeading;
	}
	else
	{
		fx = lp->pos[0] - camera_position.vx;
		fz = lp->pos[2] - camera_position.vz;

		if (fx == 0 && fz == 0)
			return JER_RESULT_CONTINUE;

		camHeading = ratan2(fx, fz);
	}

	/* stick direction relative to the screen: up = forward, right = right */
	stickHeading = ratan2(MOVE_STICK_SIGN_X * stickX, MOVE_STICK_SIGN_Y * -stickY);

	desired = (camHeading + stickHeading) & 0xfff;

	/* the writeup's "flattened to the ground plane": project the desired
	 * direction onto the SURFACE plane at Tanner's feet, so a slope with
	 * a weird normal can't make him fail to move along it — the movement
	 * follows the terrain the same way the heading blend follows the
	 * velocity vector. Flat ground (normal straight up) leaves it unchanged. */
	if (lp->pPed != NULL)
	{
		VECTOR normal;
		VECTOR surf;
		VECTOR pp;
		sdPlane* plane;
		int dx = RSIN(desired);
		int dz = RCOS(desired);
		long long dot;
		int dpx;
		int dpz;

		pp.vx = lp->pPed->position.vx;
		pp.vy = lp->pPed->position.vy;
		pp.vz = lp->pPed->position.vz;

		FindSurfaceD2(&pp, &normal, &surf, &plane);

		dot = (long long)dx * normal.vx + (long long)dz * normal.vz;

		dpx = (int)(dx - ((long long)normal.vx * dot >> 24));
		dpz = (int)(dz - ((long long)normal.vz * dot >> 24));

		if (ABS(dpx) + ABS(dpz) > 16)
			desired = ratan2(dpx, dpz);
	}

	/* diagnostics: log the heading math every 60 frames while input is
	 * live — 'stick down should make desired ~ baseDir+2048, and pdir
	 * should track dir' — so a pad-axis quirk or heading-convention
	 * error (running perpendicular to the camera) is readable at a glance */
	{
		static int gMoveLogTimer;

		if (--gMoveLogTimer <= 0)
		{
			gMoveLogTimer = 60;

			jer_log("[d2pl] move: cam=%d stick=%d desired=%d dir=%d pdir=%d ref=%d/%d\n",
				camHeading, stickHeading, desired, lp->dir,
				lp->pPed->dir.vy, gMoveRefActive, gMoveRefHeading);
		}
	}

	/* while running the turn is limited (the player arcs around, even for
	 * a 180 — a small circular path, never a snap); from standstill the
	 * pivot is near-instant so the player starts running the new way
	 * immediately */
	limit = (gPedWasMoving || ABS(lp->pPed->speed) > 4) ? RUN_TURN_LIMIT : PIVOT_TURN_LIMIT;

	/* interpolate the heading toward the target: an exponential lerp
	 * (1/MOVE_LERP_DIV of the remaining gap per frame — responsive: fast
	 * when far, eases in), capped by the run/standstill turn limits. Only
	 * applied when NOT aiming — while aiming the body follows the aim
	 * direction instead of the movement stick. */
	if (gAiming == 0)
	{
		int gap = DIFF_ANGLES(lp->dir, desired) * MOVE_TURN_SIGN;

		delta = gap / MOVE_LERP_DIV;

		if (delta == 0 && gap != 0)
			delta = (gap > 0) ? 1 : -1;	/* converge exactly */

		delta = jer_clamp_int(delta, -limit, limit);

		lp->dir = (lp->dir + delta) & 0xfff;
		lp->pPed->dir.vy = (lp->dir + 2048) & 0xfff;
	}

	/* rewrite the movement bits: clear the engine's D-pad synth + tank
	 * steer, then RUN FORWARD toward the stick heading — the body turns
	 * (lerped above) rather than ever walking backward, GTA-style */
	a->pad &= ~(TANNER_PAD_GOFORWARD | TANNER_PAD_GOBACK |
		TANNER_PAD_TURNLEFT | TANNER_PAD_TURNRIGHT);

	a->pad |= TANNER_PAD_GOFORWARD;

	gPedWasMoving = 1;

	return JER_RESULT_CONTINUE;
}

/* Smooth the camera toward its desired position with a preference for
 * responsive-but-smooth reactivity. If the desired position clips into
 * world geometry (the engine's collision query reports it), the target is
 * pushed clear toward the player and the camera moves there UNBOUNDED —
 * a fast transform is allowed when it prevents clipping, so the camera
 * never keeps rendering inside a wall. On a context jump (level switch,
 * intro sweep -> play) the camera re-places instantly; during play a
 * correction travels at the bounded MAX_POS_STEP rate (never a snap).
 * The on-foot camera is always kept above the player's ground level.
 * Returns 1 when a clip push happened (for diagnostics); *penOut, when
 * non-NULL, receives the terrain penetration (how far the desired position
 * would sink into the ground, for the forced look-up effect). */
static int D2plSmoothCamera(VECTOR* camPos, int* base, int inCar, int* penOut)
{
	int div;

	if (inCar)
		div = 4;	/* responsive smoothness (1/div toward target per
				   frame) — a car turning sharply gets a little (subtle)
				   runway lag, but tighter than before so the player can
				   still see around the turn */
	else if (!gAiming && gFootSpeedSmooth < FOOT_LAG_SPEED)
		div = FOOT_LAG_DIV;	/* on foot, not aiming, just starting to
				   move: a touch MORE lag gives him the semblance of
				   momentum as he accelerates; once he's moving the
				   camera tightens up (div 5 below) */
	else
		div = 5;
	int baseY = -base[1];	/* basePos y is RAW (un-negated); the camera frame
				   negates it (camera.c: carheight - basePos[1]) */
	int targetY = baseY + (inCar ? CAR_HEIGHT : gS.footHeight);
	int clipped = 0;
	int i;

	if (penOut != NULL)
		*penOut = 0;

	{
		int jump = ABS(gCamSmooth.vx - camPos->vx)
			+ ABS(gCamSmooth.vy - camPos->vy)
			+ ABS(gCamSmooth.vz - camPos->vz);

		if (!gCamSmoothValid || jump > CONTEXT_JUMP)
		{
			/* first frame after boot/level switch — or the target jumped
			 * more than a context change (intro sweep -> play, spawn):
			 * place it, no lerp. A fresh context, not a correction (the
			 * no-snap rule covers in-frame corrections, not context
			 * switches — flying 190k units at MAX_POS_STEP is worse). */
			gCamSmooth.vx = camPos->vx;
			gCamSmooth.vy = camPos->vy;
			gCamSmooth.vz = camPos->vz;
			gCamSmoothValid = 1;
		}
	}

	/* clipping: push the target clear; the position rig then eases there
	 * at the bounded MAX_POS_STEP rate (a failed push keeps the desired
	 * position) */
	if (CameraCollisionCheck())
	{
		int steps = 0;

		clipped = 1;

		while (steps < 8)
		{
			camPos->vx -= (camPos->vx - base[0]) / 3;
			camPos->vy -= (camPos->vy - targetY) / 3;
			camPos->vz -= (camPos->vz - base[2]) / 3;
			steps++;

			if (!CameraCollisionCheck())
				break;
		}

		if (steps >= 8)
			clipped = 2;	/* could not clear: keep the desired position */
	}

	/* move toward the target, rounding so small deltas don't stall, and
	 * clamping each axis to MAX_POS_STEP so even a huge correction
	 * travels instead of teleporting — EXCEPT on the frame the collision
	 * push cleared, where the move is unbounded: the camera must never
	 * keep rendering inside a wall */
	for (i = 0; i < 3; i++)
	{
		int* smooth = ((int*)&gCamSmooth) + i;
		int* target = ((int*)camPos) + i;
		int delta = (*target - *smooth + div / 2) / div;

		if (clipped != 1)
		{
			if (delta > MAX_POS_STEP)
				delta = MAX_POS_STEP;
			if (delta < -MAX_POS_STEP)
				delta = -MAX_POS_STEP;
		}

		*smooth += delta;
	}

	camPos->vx = gCamSmooth.vx;
	camPos->vy = gCamSmooth.vy;
	camPos->vz = gCamSmooth.vz;

	/* cap the camera to the terrain (camera frame, negated y): the camera
	 * may not sink below the road surface — clamp it back up to the
	 * surface line and report how deep it would have gone (the
	 * ground-slide effect). MapHeight returns the RAW terrain height, so
	 * in the negated camera frame the ground line is -MapHeight and
	 * 'stay CLEAR above it' = vy <= -MapHeight - CLEAR. */
	{
		int camMaxY = -(inCar ? TERRAIN_CLEAR_CAR : TERRAIN_CLEAR_FOOT) - MapHeight(camPos);

		if (camPos->vy > camMaxY)
		{
			if (penOut != NULL)
				*penOut = camPos->vy - camMaxY;

			camPos->vy = camMaxY;
		}
	}

	return clipped;
}

/* Diagnostics: log the camera state every 60 frames (and once on a clip)
 * so a broken view can be traced — position, angle, the input axes, and
 * Tanner's facing vs the camera heading (the 'running perpendicular'
 * style issues show up here: stick down should make desired = baseDir,
 * and pdir should track dir). */
static int gCamLogTimer = 0;

static void D2plLogCamera(PLAYER* lp, int baseDirIn, VECTOR* camPos,
	SVECTOR* camAngle, int clipped)
{
	gCamLogTimer--;

	if (gCamLogTimer > 0 && clipped == 0)
		return;

	gCamLogTimer = 30;

	jer_log("[d2pl] cam: pos=(%d,%d,%d) ang=(%d,%d) scr_z=%d collide=%d | "
		"tanner: dir=%d pdir=%d pos=(%d,%d,%d) | stick=%d,%d | "
		"camYaw=%d baseDir=%d ref=%d/%d\n",
		camPos->vx, camPos->vy, camPos->vz,
		camAngle->vx, camAngle->vy, scr_z, clipped,
		lp->dir, (lp->pPed != NULL) ? lp->pPed->dir.vy : -9999,
		lp->pos[0], lp->pos[1], lp->pos[2],
		(lp->padid >= 0) ? Pads[lp->padid].mapanalog[0] : -9999,
		(lp->padid >= 0) ? Pads[lp->padid].mapanalog[1] : -9999,
		lp->cameraAngle, baseDirIn,
		gMoveRefActive, gMoveRefHeading);
}

/* The orbit looks INWARD: point the view at the focal point from the final
 * camera position, using the engine's own PointAtTarget so the convention
 * is guaranteed to match the render. On foot the focal point is the player.
 * In a vehicle the focal point sits a little ABOVE the car (orbit point up)
 * and, when the player is not free-looking, AHEAD of the car — so the
 * settled camera points where you're driving. Releasing the look stick
 * blends the focal point back to the forward point smoothly. */
static int gAimLookBlend = 0;	/* 0 = settled (forward), 4096 = free-looking */

static void D2plAimAtPlayer(SVECTOR* camAngle, VECTOR* camPos, int* base,
	int inCar, int baseDir, int camYaw, int gripping)
{
	VECTOR target;
	int baseY = -base[1];	/* camera-frame anchor y (base y is RAW) */

	target.vx = base[0];
	target.vy = baseY;
	target.vz = base[2];

	if (inCar)
	{
		VECTOR carPoint;
		VECTOR aheadPoint;
		int diff;
		int blendTarget;

		/* orbit/focal point: above the car's base in the CAMERA frame
		 * (base y is RAW; up = -y). The old raw-frame focal sat 160
		 * units below the car in the camera frame, so the view pitched
		 * down at a point under the car and the car drifted out of the
		 * frame — now it aims at roof height. */
		carPoint.vx = base[0];
		carPoint.vy = baseY + CAR_ORBIT_UP;
		carPoint.vz = base[2];

		/* settled focal point: ahead of the car, where you're driving */
		aheadPoint.vx = base[0] + FIXEDH(RSIN(baseDir) * CAR_LOOK_AHEAD);
		aheadPoint.vy = baseY + CAR_ORBIT_UP;
		aheadPoint.vz = base[2] + FIXEDH(RCOS(baseDir) * CAR_LOOK_AHEAD);

		/* gravitate toward looking AHEAD of the car only very close to the
		 * settled angle, and onto the car itself much earlier while
		 * view-panning: the ahead zone spans ~5.6 degrees and falls off
		 * fully to the car by ~22.5 degrees, so plenty of angles that
		 * should look at the car do. While the stick is actually being
		 * held (gripping) the focal snaps STRAIGHT to the car — a pure
		 * spherical pan, no stretching toward the ahead point. The settled
		 * orbit angle is baseDir + gCameraAngle (the camera parked BEHIND
		 * the car, facing forward — camera.c:560); measure against THAT. */
		if (gripping)
		{
			blendTarget = 4096;	/* panning: look at the car, always */
		}
		else
		{
			diff = ABS(jer_angle_diff(camYaw, (baseDir + gCameraAngle) & 0xfff));

			if (diff <= 64)
				blendTarget = 0;	/* settled: look ahead of the car */
			else if (diff >= 256)
				blendTarget = 4096;	/* panning: look at the car */
			else
				blendTarget = ((diff - 64) * 4096) / 192;
		}

		gAimLookBlend = jer_lerp_int(gAimLookBlend, blendTarget, 4);

		target.vx = jer_lerp(aheadPoint.vx, carPoint.vx, gAimLookBlend);
		target.vy = jer_lerp(aheadPoint.vy, carPoint.vy, gAimLookBlend);
		target.vz = jer_lerp(aheadPoint.vz, carPoint.vz, gAimLookBlend);
	}
	else
	{
		gAimLookBlend = 0;

		/* aim at the torso: the chest sits FOOT_AIM_HEIGHT above the
		 * waist anchor in the camera frame (negated, so up = -y) — the
		 * camera sits a little low and looks up at his chest */
		target.vy = baseY - FOOT_AIM_HEIGHT;
	}

	PointAtTarget(camPos, &target, camAngle);
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

	/* kill-switch: stock camera when disabled */
	if (gS.cameraEnabled == 0)
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

		/* rebase into the module (negated) frame like the other branches —
		 * the engine hands the aim path RAW y (camera.c:570), and the
		 * shared smooth/cap/collision math below expects -rawY */
		camPos->vy = -((int*)a->basePos)[1] + AIM_HEIGHT;

		camPos->vx += FIXEDH(RCOS(heading) * AIM_LATERAL * gS.shoulder);
		camPos->vz += -FIXEDH(RSIN(heading) * AIM_LATERAL * gS.shoulder);

		if (a->basePos != NULL)
		{
			int clip = D2plSmoothCamera(camPos, (int*)a->basePos, a->inCar, NULL);

			D2plLogCamera(lp, a->baseDir, camPos, camAngle, clip);
		}

		SetGeomScreen(scr_z = gCameraDefaultScrZ + AIM_ZOOM);

		gCurrentWeapon->aimPoint(gAimPoint);

		a->override = 1;

		return JER_RESULT_CONTINUE;
	}

	/* --- neutral camera: base FOV override + pitch link + speed lens. The
	 * pitch is the camera's ELEVATION around the player (up lifts the
	 * camera, so the view looks DOWN at the player from above); looking
	 * down from above narrows the FOV (GTA4-style), looking up from below
	 * widens it. Smoothed speed adds a subtle extra widening (GTA IV §5). --- */
	{
		int fovScrZ = gS.fovEnabled ? D2plFovScrZ(gS.fovDeg) : gCameraDefaultScrZ;

		SetGeomScreen(scr_z = fovScrZ + (gLookPitch * FOV_PITCH_SCALE) / 128
			- (gSpeedSmooth * FOV_SPEED_SCALE) / 4096);

		if (scr_z < 96)
			scr_z = 96;	/* the speed lens must never collapse the lens */
	}

	if (lp->playerCarId >= 0)
	{
		/* --- GTA4-style chase cam for vehicles --- */
		int* base = (int*)a->basePos;	/* RAW y: rebased below into the
						   camera (negated) frame */
		CAR_COSMETICS* car_cos = car_data[lp->playerCarId].ap.carCos;
		int lateral = 120;
		int speed = gSpeedSmooth;	/* smoothed: no distance jitter (§5) */
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

		/* rebase y into the module frame (vy = -rawY) so the terrain cap,
		 * collision push, aim target and pitch-orbit anchor all agree;
		 * the engine's own carheight - basePos[1] frame (per-car ride
		 * height) was silently fighting the cap on flat ground */
		camPos->vy = -base[1] + CAR_HEIGHT - (gSpeedSmooth * HEIGHT_SPEED_SCALE) / 4096;

		camPos->vx += FIXEDH(RCOS(heading) * lateral * gS.shoulder);
		camPos->vz += -FIXEDH(RSIN(heading) * lateral * gS.shoulder);
	}
	else
	{
		/* --- on foot: place the camera a proper TPS distance behind the
		 * ped (he anchors ~130 units above the ground — AnimatePed — so
		 * the framing is ~1 ped-length back, ~1/4 length to the side,
		 * slightly below the body centre). --- */
		int* base = (int*)a->basePos;

		if (base != NULL)
		{
			/* the distance modulates with run speed: pulls in when
			 * stationary/walking, extends when running (framing doc) */
			int footDist = gS.footDist
				+ (gFootSpeedSmooth - FOOT_WALK_SPEED) * FOOT_SPEED_DIST;

			camPos->vx = base[0] + (-FIXEDH(RSIN(heading) * footDist)
				+ FIXEDH(RCOS(heading) * gS.footLat * gS.shoulder));
			camPos->vz = base[2] + (-FIXEDH(RCOS(heading) * footDist)
				- FIXEDH(RSIN(heading) * gS.footLat * gS.shoulder));
			camPos->vy = -base[1] + gS.footHeight;	/* base y is RAW: negate
							   it to the camera frame */

			/* subtle view bob/sway while Tanner moves (still when idle) */
			{
				int speed = ABS(lp->pos[0] - gBobLastX) + ABS(lp->pos[2] - gBobLastZ);

				gBobLastX = lp->pos[0];
				gBobLastZ = lp->pos[2];

				if (speed > 8)
				{
					camPos->vy += FIXEDH(RSIN(FrameCnt * 3) * 20);
					camPos->vx += FIXEDH(RCOS(FrameCnt * 2) * 12);
				}
			}
		}
	}

	/* --- "orbit looking INWARD": pushing up LIFTS the camera up around the
	 * player/vehicle (a true sphere orbit — elevation), so you see them from
	 * above; pushing down drops it below them. Position only: the offset
	 * vector is rotated by the elevation angle in the vertical plane, then
	 * the view is aimed straight back at the player below. --- */
	if (gLookPitch != 0 && a->basePos != NULL)
	{
		int* base = (int*)a->basePos;
		int baseY = -base[1];	/* camera-frame anchor y (base y is RAW) */
		int ox = camPos->vx - base[0];
		int oz = camPos->vz - base[2];
		int oy = camPos->vy - baseY;
		int cosP = RCOS(gLookPitch);		/* cos(elevation): 0..4096 */
		int sinP = RSIN(gLookPitch);		/* -sin(elevation) */
		int distH = SquareRoot0(ox * ox + oz * oz);

		if (distH > 0)
		{
			/* rotate the offset (ox, oy, oz) around the horizontal axis by
			 * the elevation: h' = h*cosP + oy*sinP, oy' = oy*cosP - distH*sinP.
			 * Sphere rotation — the distance to the player never changes.
			 * The vertical->horizontal spill (hShift) is a small correction;
			 * at extreme pitch distH shrinks toward zero and dividing by it
			 * blows the camera away (the "freaks out at full stick" bug),
			 * so it is clamped and only applied while the correction stays
			 * small relative to the horizontal distance. */
			{
				int hShift = (oy * sinP) >> 12;	/* vertical -> horizontal spill */

				if (hShift > 64)
					hShift = 64;
				if (hShift < -64)
					hShift = -64;

				camPos->vx = base[0] + FIXEDH(ox * cosP);
				camPos->vz = base[2] + FIXEDH(oz * cosP);
				camPos->vy = baseY + FIXEDH(oy * cosP) - FIXEDH(distH * sinP);

				/* |hShift*ox/distH| <= |ox|/4 while distH > 4*|hShift| */
				if (distH > 4 * ABS(hShift))
				{
					camPos->vx += (hShift * ox) / distH;
					camPos->vz += (hShift * oz) / distH;
				}
			}
		}
	}

	/* smooth the final position (responsive but smooth; faster + pushed
	 * clear when it would clip) and aim the view straight at the player */
	if (a->basePos != NULL)
	{
		int* base = (int*)a->basePos;
		int pen = 0;
		int clip = D2plSmoothCamera(camPos, base, a->inCar, &pen);

		D2plAimAtPlayer(camAngle, camPos, base, a->inCar, a->baseDir,
			lp->cameraAngle, (gLookIdle < 4 || gYawSpeed != 0));

		/* ground-slide forced look-up: the harder the terrain cap pins the
		 * camera (deep would-be intersection), the more the view tilts up —
		 * a small fraction of the penetration depth, capped. */
		if (pen > 0)
		{
			int boost = (pen * GROUND_LOOKUP_SCALE) / 256;

			if (boost > GROUND_LOOKUP_MAX)
				boost = GROUND_LOOKUP_MAX;

			camAngle->vx -= boost;
		}

		D2plLogCamera(lp, a->baseDir, camPos, camAngle, clip);
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
			w->poseArmAim(a->skel, ((LPPEDESTRIAN)a->ped)->dir.vy);
		else
			w->poseArm(a->skel, ((LPPEDESTRIAN)a->ped)->dir.vy);
	}
	else if (a->phase == 1 && !a->shadow)
	{
		/* one-shot per-level diagnostic: the ped's real world scale vs the
		 * camera framing, so the on-foot camera units stay in sync with
		 * the ped (Driver's inverted y makes vy extent the height) */
		if (!gPedScaleLogged)
		{
			int i;
			int minX = 32767, maxX = -32767, minY = 32767, maxY = -32767;
			int minZ = 32767, maxZ = -32767;

			for (i = 0; i < 23; i++)
			{
				if (vJPos[i].vx < minX) minX = vJPos[i].vx;
				if (vJPos[i].vx > maxX) maxX = vJPos[i].vx;
				if (vJPos[i].vy < minY) minY = vJPos[i].vy;
				if (vJPos[i].vy > maxY) maxY = vJPos[i].vy;
				if (vJPos[i].vz < minZ) minZ = vJPos[i].vz;
				if (vJPos[i].vz > maxZ) maxZ = vJPos[i].vz;
			}

			jer_log("[d2pl] ped: head_pos=%d skel X[%d..%d] Y[%d..%d] Z[%d..%d] H=%d W=%d camDelta=(%d,%d) player=(%d,%d,%d)\n",
				((LPPEDESTRIAN)a->ped)->head_pos,
				minX, maxX, minY, maxY, minZ, maxZ,
				maxY - minY, maxX - minX,
				cameraPos->vx - playerPos->vx, cameraPos->vz - playerPos->vz,
				playerPos->vx, playerPos->vy, playerPos->vz);

			gPedScaleLogged = 1;
		}

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

	/* one-shot pad diagnostics after the pads are mapped (type is 0 at
	 * INIT — MapPad fills it on the first update frames) */
	{
		static int gPad0Logged = 0;

		if (!gPad0Logged && FrameCnt > 60)
		{
			gPad0Logged = 1;
			jer_log("[d2pl] pad0: type=%d analog=%s (id 0x41 digital / 0x73 analog)\n",
				Pads[0].type, (Pads[0].type & 4) != 0 ? "ON" : "OFF");
		}
	}

	/* -onfoot test harness: the level spawns the player in a car; once the
	 * world is playable we imperceptibly swap that spawn car out for the
	 * player on foot — activate the pedestrian beside the car, switch
	 * control, then delete the car. One-shot, retried until it succeeds
	 * (the loop can stall during level spooling). */
	extern int gBootOnFoot;

	if (gBootOnFoot && FrameCnt > 10 && lp->playerCarId >= 0)
	{
		CAR_DATA* cp;

		cp = &car_data[lp->playerCarId];

		/* activate the pedestrian beside the car first; only swap control
		 * and delete the car when the ped is actually there, and only then
		 * consume the flag (retried next frame otherwise — a failed
		 * activation must never reach ChangeCarPlayerToPed, which would
		 * deref a NULL ped) */
		ActivatePlayerPedestrian(cp, NULL, 0, NULL, TANNER_MODEL);

		if (lp->pPed != NULL)
		{
			jer_log("[d2pl] -onfoot: swapping spawn car for the player on foot\n");

			ChangeCarPlayerToPed(0);
			PingOutCar(cp);		/* delete the spawn car */

			gBootOnFoot = 0;
		}
	}

	if (lp->padid >= 0)
	{
		pad = Pads[lp->padid].mapped;
		padNew = Pads[lp->padid].mapnew;
	}

	/* mouse integration: read EVERY frame so the relative motion and the
	 * button edges never go stale; the button mapping only applies on
	 * foot. INPUT-DEVICE AUTHORITY: the mouse only drives the view while
	 * it is the recent input device — motion/click grants ~0.75 s of
	 * authority, live controller analog (the keyboard leaves the analog
	 * channels at 0) revokes it immediately. Right-click aims (L1),
	 * left-click fires (R1), middle fires secondary (R2). */
	{
		int mdx, mdy;
		Uint32 mbuttons = SDL_GetRelativeMouseState(&mdx, &mdy);
		Uint32 mnew = mbuttons & ~gLastMouseButtons;

		gMouseDX = mdx;
		gMouseDY = mdy;

		if (mdx != 0 || mdy != 0 ||
			(mbuttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK)) != 0)
		{
			gMouseActive = MOUSE_AUTHORITY_FRAMES;
		}
		else if (gMouseActive > 0)
		{
			int p = lp->padid >= 0 ? lp->padid : 0;
			int anyAnalog = ABS(Pads[p].mapanalog[0]) > 8
				|| ABS(Pads[p].mapanalog[1]) > 8
				|| ABS(Pads[p].mapanalog[2]) > 8
				|| ABS(Pads[p].mapanalog[3]) > 8;

			if (anyAnalog)
				gMouseActive = 0;	/* the controller took over */
			else
				gMouseActive--;
		}

		/* no authority: never let stale motion reach the look handler */
		if (gMouseActive == 0)
		{
			gMouseDX = 0;
			gMouseDY = 0;
		}

		if (lp->playerCarId < 0 && gMouseActive > 0)
		{
			if ((mbuttons & SDL_BUTTON_RMASK) != 0)
				pad |= MPAD_L1;
			if ((mbuttons & SDL_BUTTON_LMASK) != 0)
				pad |= MPAD_R1;
			if ((mbuttons & SDL_BUTTON_MMASK) != 0)
				pad |= MPAD_R2;

			if ((mnew & SDL_BUTTON_LMASK) != 0)
				padNew |= MPAD_R1;
			if ((mnew & SDL_BUTTON_MMASK) != 0)
				padNew |= MPAD_R2;
		}

		gLastMouseButtons = mbuttons;
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
		/* laser-sight line from Tanner's hand to the evaluated aim point
		 * while aiming. Depth-sorted: it respects Z order (walls occlude
		 * it). The hand/aim positions are in the render (negated) frame
		 * while Debug_AddLine expects y-up world coords — negate y here. */
	if (gAiming && gS.laserColor != D2PL_LASER_OFF && gCurrentWeapon != NULL &&
		(gHandPos[0] != 0 || gHandPos[1] != 0 || gHandPos[2] != 0))
	{
		VECTOR from;
		VECTOR to;
		CVECTOR col;

		D2plLaserColor(&col);

		from.vx = gHandPos[0];
		from.vy = -gHandPos[1];
		from.vz = gHandPos[2];

		to.vx = gAimPoint[0];
		to.vy = -gAimPoint[1];
		to.vz = gAimPoint[2];

		Debug_AddLineDepth(from, to, col);
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
		c.vy = -gMarkerPos[1];	/* render frame -> y-up for Debug_AddLine */
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
	case D2PL_ITEM_JOYSTICK:
		sprintf(gD2plLabelBuf[item], "Joystick Look: %s",
			gS.joyCamera ? "ON" : "OFF");
		break;
	case D2PL_ITEM_FOOT_DIST:
		sprintf(gD2plLabelBuf[item], "Foot Distance: %d", gS.footDist);
		break;
	case D2PL_ITEM_FOOT_LAT:
		sprintf(gD2plLabelBuf[item], "Foot Offset: %d", gS.footLat);
		break;
	case D2PL_ITEM_FOOT_HEIGHT:
		sprintf(gD2plLabelBuf[item], "Foot Height: %d", gS.footHeight);
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
	case D2PL_ITEM_CAMERA:
		sprintf(gD2plLabelBuf[item], "Camera Mod: %s",
			gS.cameraEnabled ? "ON" : "OFF");
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
	case D2PL_ITEM_JOYSTICK:
		if (direction != 0)
			gS.joyCamera = !gS.joyCamera;
		break;
	case D2PL_ITEM_FOOT_DIST:
		gS.footDist = jer_clamp_int(gS.footDist + direction * 40, 80, 1600);
		break;
	case D2PL_ITEM_FOOT_LAT:
		gS.footLat = jer_clamp_int(gS.footLat + direction * 10, 0, 300);
		break;
	case D2PL_ITEM_FOOT_HEIGHT:
		gS.footHeight = jer_clamp_int(gS.footHeight - direction * 10, -500, 0);
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
	case D2PL_ITEM_CAMERA:
		if (direction != 0)
			gS.cameraEnabled = !gS.cameraEnabled;
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
	gYawSpeed = 0;
	gGripDist = 0;
	gMoveRefHeading = 0;
	gMoveRefActive = 0;
	gVelAngle = 0;
	gSpeedSmooth = 0;
	gLastBaseX = 0;
	gLastBaseZ = 0;
	gBobLastX = 0;
	gBobLastZ = 0;
	gLookBackHeld = 0;
	gCamInitialized = 0;
	gSettleInfluence = 0;
	gAngRate = 0;
	gLastCarDir = 0;
	gInstability = 0;
	gInstabConfirm = 0;
	gTrackedCarId = -2;
	gFootSpeedSmooth = 0;
	gCamSmoothValid = 0;	/* level start: the first camera frame re-places */
	gLastTannerPad = 0;
	gPedWasMoving = 0;
	gAimLookBlend = 0;

	gHandPos[0] = gHandPos[1] = gHandPos[2] = 0;
	gAimPoint[0] = gAimPoint[1] = gAimPoint[2] = 0;
	gAiming = 0;
	gMarkerTimer = 0;
	gPedScaleLogged = 0;

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

	ctx->jer_log(ctx, "[d2pl] Driver 2 Parallel Lines ready (%d weapon(s): %s; sens %d/%d, foot %d/%d/%d, car %d/%d%%, shoulder %s, fov %d(%s), laser %s)\n",
		gWeaponCount,
		gCurrentWeapon != NULL ? gCurrentWeapon->id : "none",
		gS.sensX, gS.sensY,
		gS.footDist, gS.footLat, gS.footHeight,
		gS.carDist, gS.carLatPct,
		gS.shoulder > 0 ? "right" : "left",
		gS.fovDeg, gS.fovEnabled ? "on" : "off",
		gLaserNames[gS.laserColor]);
}
