/*
 * d2pl_ped.c — Driver 2 Parallel Lines (on-foot movement (camera-relative run-forward), on-foot camera framing).
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

/* --- ped-related shared state (defined here; extern in d2pl.h) --- */

int gBobLastX;	/* last on-foot base position (view bob motion) */
int gBobLastZ;
int gFootSpeedSmooth;	/* smoothed on-foot speed (pPed->speed) */
int gLastTannerPad;	/* previous frame's on-foot pad — the hook carries
				   no padNew, so the X-reload edge is tracked here */
int gPedWasMoving;

/* weapon state */

/* ================================================================== */
/* JER_EVENT_PED_INPUT - camera-relative movement + the X-button reload   */
/* ================================================================== */

int D2plOnPedInput(void* userdata, void* args)
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

/* ================================================================== */
/* D2plPedCamera - the on-foot TPS framing (distance modulates with       */
/* run speed; subtle view bob while moving)                               */
/* ================================================================== */

void D2plPedCamera(void* args, int heading)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	PLAYER* lp = (PLAYER*)a->player;
	VECTOR* camPos = (VECTOR*)a->cameraPosition;

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
