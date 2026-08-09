/*
 * d2pl_main.c — Driver 2 Parallel Lines (module entry, settings/config/pause shell, shared camera core).
 */
#include "d2pl.h"

#include "players.h"
#include "cars.h"
#include "camera.h"
#include "pad.h"
#include "dr2math.h"
#include "main.h"
#include "draw.h"
#include "models.h"
#include "pres.h"
#include "pedest.h"
#include "civ_ai.h"
#include "dr2roads.h"
#include "PsyX/PsyX_public.h"

#include <math.h>

/* the debug line API used by the engine's collision overlays (PC only) */
#ifndef PSX
extern void Debug_AddLine(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
/* JERICHO-HOOK (DebugOverlay.cpp): depth-sorted variant — the line joins
 * the world's ordering table at its Z bucket, so walls occlude it */
extern void Debug_AddLineDepth(VECTOR& pointA, VECTOR& pointB, CVECTOR& color);
#endif

/* ================================================================== */
/* Settings (persisted via the JERICHO config API; schema in d2pl.h)     */
/* ================================================================== */

static void D2plSaveSettings(void);	/* forward (load regenerates on stale) */

D2PL_SETTINGS gS = {
	21,			/* sensX — baseline sensitivity, ~10% under the 64
				   reference (the persisted config is regenerated) */
	18,			/* sensY */
	1,			/* joyCamera — right-stick look/orbit on by default */
	910,			/* footDist — doubled: a high, wide GTA-like view that
				   can pitch down steeper and show more around the ped */
	120,			/* footLat (shoulder) */
	-240,			/* footHeight — the orbit moved up (camera ~300 units
				   above the waist anchor, still aiming at the player) */
	575,			/* carDist (+15% default distance) */
	28,			/* carLatPct */
	-108,			/* carHeight (in-car camera lift; adjustable for
				   taller cars via the settings menu) and remember Y is inverted in this engine so negative = higher */
	-1,			/* shoulder (left) */
	0,			/* invertH */
	1,			/* invertV */
	1,			/* fovEnabled */
	82,			/* fovDeg (+10 from the old 72 default) */
	D2PL_LASER_DEFAULT,	/* laserColor (red) */
	1			/* cameraEnabled */
};

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
		gS.carHeight = jer_config_get_int(D2PL_CFG_MOD, "car_height", gS.carHeight);
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
	gS.carHeight = jer_clamp_int(gS.carHeight, -250, 50);
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
	jer_config_set_int(D2PL_CFG_MOD, "car_height", gS.carHeight);
	jer_config_set_int(D2PL_CFG_MOD, "shoulder", gS.shoulder);
	jer_config_set_int(D2PL_CFG_MOD, "invert_h", gS.invertH);
	jer_config_set_int(D2PL_CFG_MOD, "invert_v", gS.invertV);
	jer_config_set_int(D2PL_CFG_MOD, "fov_enabled", gS.fovEnabled);
	jer_config_set_int(D2PL_CFG_MOD, "fov_deg", gS.fovDeg);
	jer_config_set_int(D2PL_CFG_MOD, "laser_color", gS.laserColor);
	jer_config_set_int(D2PL_CFG_MOD, "camera_enabled", gS.cameraEnabled);
}


/* ================================================================== */
/* Shared camera/mouse state (defined here; extern in d2pl.h)             */
/* ================================================================== */

int gLookIdle;		/* frames since the stick was last used */
int gLookPitch;		/* smoothed pitch */
int gLookPitchTarget;
int gYawSpeed;		/* current orbit speed (momentum: ramps toward
				   the stick's max over ~1/3 s, decays on release) */
int gGripDist;		/* orbit radius frozen for the whole pan (the
				   spherical-pan grip) — hoisted so level resets can
				   clear it */
int gMoveRefHeading;	/* camera heading frozen at view-manipulation start
				   (the on-foot movement reference frame, GTA IV §8) */
int gMoveRefActive;	/* 1 while that freeze is in effect */
int gCamInitialized;	/* one-shot: snap the spawn orbit angle behind */
int gSettleInfluence;	/* natural-settle influence fade (0..4096): ramps
				   in on release so the ease eases IN — no correction
				   snap at the handoff, live target throughout */
int gSettlePitch0;	/* the pitch held when the settle began — it
				   returns to level IN SYNC with the angle fade
				   (the old fast 1/6 pitch snap-back was what made
				   follow <-> orbit switching feel disjointed) */
VECTOR gCamSmooth;	/* smoothed camera position (live target + bounded
				   rate; hoisted so level starts re-place it) */
int gCamSmoothValid;

/* ped was moving last frame (run-vs-pivot turn limit) */
int gMouseDX = 0;	/* relative mouse motion this frame (look) */
int gMouseDY = 0;
Uint32 gLastMouseButtons = 0;	/* for synthesized press edges */
int gMouseActive = 0;	/* frames of remaining mouse authority */
int gLookForceSettle = 0;	/* a L2/R2/L3 look-button release returns
				   the camera to the normal view immediately —
				   no hold-window delay, and past the standstill
				   reverse-grace (armed while the button is held) */
int gLookBackActive = 0;	/* 1 while the R3/L2+R2 look-behind is held */


/* ================================================================== */
/* D2plFovScrZ - invert the user-facing FOV degrees into the GTE lens     */
/* ================================================================== */

int D2plFovScrZ(int degrees)
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

/* ================================================================== */
/* JER_EVENT_CAMERA_LOOK - the two-state orbit authority                  */
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
	D2plTrackInstability(lp, inCar);

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
		gLookForceSettle = 0;	/* a pan supersedes an armed look-release */
		gLookBackActive = 0;	/* ... and any look-behind */

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
			int yawMax = (CAM_ORBIT_SPEED_MAX * gS.sensX) / 64;
			int pitchMax = (CAM_PITCH_MAX * gS.sensY) / 64;
			int sX = gS.invertH ? -stickX : stickX;
			int sY = gS.invertV ? -stickY : stickY;
			int aimDiv = gAiming ? AIM_LOOK_DIV : 1;	/* slower while aiming */
			int targetSpeed;

			/* orbit: the angle moves by a momentum-driven speed. The speed
			 * ramps from 0 up to the stick's max (proportional to deflection)
			 * over ~1/6 of a second (CAM_ORBIT_RAMP_FRAMES frames) — the view "gains
			 * momentum" instead of jumping to full speed. The orbit stays
			 * gripped so the engine's settle-back lerp can't fight it;
			 * vertical is clamped below. */
			gLookIdle = 0;
			a->suppress = 1;
		a->gripOrbit = 1;

		targetSpeed = (LOOK_YAW_SIGN * sX * yawMax) / (127 * aimDiv);

		/* ramp with a minimum step of 1 so tiny deflections still move */
		{
			int step = (targetSpeed - gYawSpeed) / CAM_ORBIT_RAMP_FRAMES;

			if (step == 0 && targetSpeed != gYawSpeed)
				step = (targetSpeed > gYawSpeed) ? 1 : -1;

			gYawSpeed += step;
		}

		lp->cameraAngle = (lp->cameraAngle + gYawSpeed) & 0xfff;

		/* clamp so the camera can never swing past straight up/down even at
		 * the highest sensitivity setting */
		pitchMax = jer_clamp_int(pitchMax, 0, 900);

		gLookPitchTarget = jer_clamp_int(
			(CAM_PITCH_STICK_SIGN * sY * pitchMax) / (128 * aimDiv), -pitchMax, pitchMax);

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
				(CAM_PITCH_STICK_SIGN * mY * gS.sensY * MOUSE_PITCH) / (64 * 128 * aimDiv),
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
		gLookForceSettle = 1;	/* armed: the release returns immediately */
		gLookBackActive = lookBack;

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
			int target = (natural + gCameraAngle + side * CAM_LOOK_SIDE_ANGLE) & 0xfff;

			lp->cameraAngle = jer_lerp_angle(lp->cameraAngle, target, CAM_LOOK_SIDE_DIV);
		}
	}
	else
	{
		/* stick released: the momentum decays quickly so the settle-back
		 * doesn't jerk; a vehicle in motion resets almost immediately */
		gYawSpeed -= gYawSpeed / 8;

		if (ABS(gYawSpeed) < 8)
			gYawSpeed = 0;	/* kill the residual truncation drift */

		if (gLookForceSettle)
		{
			/* a look-button release: return to the normal camera WITHOUT
			 * the usual hold delay (that delay is for the free-look pan —
			 * a deliberate L2/R2 view returns at once) */
			gLookIdle = 10000;
		}
		else
		{
			gLookIdle++;
		}

		gLookBackActive = 0;	/* any released frame clears the look-behind */

		{
			int settleFrames = inCar
				? (D2plCarSpeed(lp) > CAM_LOW_SPEED_GRIP
					? CAM_SETTLE_IDLE_CAR_MOVING : CAM_SETTLE_IDLE_CAR)
				: CAM_SETTLE_IDLE_FOOT;

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
			else if (inCar && D2plCarSpeed(lp) <= CAM_LOW_SPEED_GRIP && gLookForceSettle == 0)
			{
				/* reverse grace: hold at standstill — EXCEPT right after a
				 * look-button release, which must return immediately
				 * (gLookForceSettle is cleared by the settle below) */
				a->suppress = 1;
				a->gripOrbit = 1;
			}
			else
			{
				int natural = D2plNaturalHeading(lp, inCar);
				int delta;

				gLookForceSettle = 0;	/* the forced return ran: re-arm */

				/* influence fade (the shared underlying trick): ramp how
				 * much of the live target is applied from 0 -> 1 instead of
				 * starting a fresh point-to-point blend — the target kept
				 * computing during the pan, so there is no correction snap */
				if (gSettleInfluence < 64)
					gSettlePitch0 = gLookPitchTarget;	/* first settle frame
									   (capture BEFORE the
									   ramp: 0 -> 256 on the
									   first step) */

				gSettleInfluence += (4096 - gSettleInfluence) / 16;

				delta = (DIFF_ANGLES(lp->cameraAngle, (natural + gCameraAngle) & 0xfff)
					* gSettleInfluence) / SETTLE_DIV / 4096;

				lp->cameraAngle = (lp->cameraAngle + delta) & 0xfff;

				a->suppress = 1;
				a->gripOrbit = 1;	/* the module owns the settle now */
				gLookPitchTarget = (gSettlePitch0 * (4096 - gSettleInfluence)) / 4096;
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


/* ================================================================== */
/* Smooth the camera toward its desired position (bounded rate,          */
/* collision push-clear, terrain cap)                                     */
/* ================================================================== */

int D2plSmoothCamera(VECTOR* camPos, int* base, int inCar, int* penOut)
{
	int div;

	if (inCar)
		div = 2;	/* responsive smoothness (1/div toward target per
				   frame) — a car turning sharply gets a little (subtle)
				   runway lag, but tighter than before so the player can
				   still see around the turn */
	else if (!gAiming && gFootSpeedSmooth < CAM_FOOT_LAG_SPEED)
		div = CAM_FOOT_LAG_DIV;	/* on foot, not aiming, just starting to
				   move: a touch MORE lag gives him the semblance of
				   momentum as he accelerates; once he's moving the
				   camera tightens up (div 5 below) */
	else
		div = 4;
	int baseY = -base[1];	/* basePos y is RAW (un-negated); the camera frame
				   negates it (camera.c: carheight - basePos[1]) */
	int targetY = baseY + (inCar ? gS.carHeight : gS.footHeight);
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

		/* push the camera clear of the collision toward the player.
		 * SKIPPED while looking behind: the push would slam the front
		 * camera onto the car (the pursuit "zoom focus") — the rear view
		 * is momentary, so brief clipping is preferable to the camera
		 * zooming into the car's face */
		while (steps < 8 && gLookBackActive == 0)
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


void D2plLogCamera(PLAYER* lp, int baseDirIn, VECTOR* camPos,
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

/* ================================================================== */
/* JER_EVENT_CAMERA - dispatch to the theme placements                    */
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

	/* aiming: the weapon module owns the whole placement (over the
	 * shoulder, camera-relative aim, narrower FOV) */
	if (gAiming && gCurrentWeapon != NULL && lp->playerCarId < 0)
		return D2plAimCamera(args, heading);

	/* --- neutral camera: base FOV override + pitch link + speed lens. The
	 * pitch is the camera's ELEVATION around the player (up lifts the
	 * camera, so the view looks DOWN at the player from above); looking
	 * down from above narrows the FOV (GTA4-style), looking up from below
	 * widens it. Smoothed speed adds a subtle extra widening (GTA IV §5). --- */
	{
		int fovScrZ = gS.fovEnabled ? D2plFovScrZ(gS.fovDeg) : gCameraDefaultScrZ;

		SetGeomScreen(scr_z = fovScrZ + (gLookPitch * CAM_FOV_PITCH_SCALE) / 128
			- (gSpeedSmooth * FOV_SPEED_SCALE) / 4096);

		if (scr_z < 96)
			scr_z = 96;	/* the speed lens must never collapse the lens */
	}


	if (lp->playerCarId >= 0)
		D2plCarCamera(args, heading);
	else
		D2plPedCamera(args, heading);

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


/* ================================================================== */
/* JER_EVENT_FRAME - pad diagnostics, -onfoot harness, mouse authority,   */
/* then the weapon frame update                                          */
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

	/* the weapon state (aim toggle, laser, marker, fire/reload) */
	D2plWeaponFrame(lp, pad, padNew);

	return JER_RESULT_CONTINUE;
}

/* ================================================================== */
/* Pause-menu bridge (labels + adjustment)                               */
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
	case D2PL_ITEM_CAR_HEIGHT:
		sprintf(gD2plLabelBuf[item], "Car Height: %d", gS.carHeight);
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
	case D2PL_ITEM_CAR_HEIGHT:
		gS.carHeight = jer_clamp_int(gS.carHeight - direction * 10, -250, 50);
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

	//D2plSaveSettings();
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


/* ================================================================== */
/* Reset + game-start hook                                               */
/* ================================================================== */

void D2plReset(void)
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
	gLookForceSettle = 0;
	gLookBackActive = 0;
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
	gPedRunSpeed = 0;
	gPedStickMove = 0;
	gAimLookBlend = 0;
	gSettlePitch0 = 0;

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


int D2plOnGameStart(void* userdata, void* args)
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

/* ================================================================== */
/* Module entry: register every hook once                                */
/* ================================================================== */

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

	/* build the demo pistol and register it (d2pl_weapon.c) */
	D2plWeaponInit();

	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA_LOOK, D2plOnLook, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_CAMERA, D2plOnCamera, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_INPUT, D2plOnPedInput, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_MOVE, D2plOnPedMove, NULL, 0);
	ctx->jer_register_hook(ctx, JER_EVENT_PED_POSE, D2plOnPedPose, NULL, 0);
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
