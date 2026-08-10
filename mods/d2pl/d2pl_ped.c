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
int gPedRunSpeed;	/* smoothed run speed 0..40 (the analog
				   magnitude, lerped — the press-off momentum) */
int gPedStickMove;	/* 1 while the left stick drives the movement */

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
		gPedStickMove = 0;	/* the stick is not the movement source: the
					   stock D-pad/keyboard handling stays */
		return JER_RESULT_CONTINUE;	/* stock handling */
	}

	gPedStickMove = 1;	/* the stick drives the movement: the momentum
				   speed is the source of truth this frame */

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

	/* rewrite the movement bits: clear the engine's D-pad synth + tank
	 * steer, then RUN FORWARD toward the stick heading — the body turns
	 * (lerped above) rather than ever walking backward, GTA-style */
	a->pad &= ~(TANNER_PAD_GOFORWARD | TANNER_PAD_GOBACK |
		TANNER_PAD_TURNLEFT | TANNER_PAD_TURNRIGHT);

	a->pad |= TANNER_PAD_GOFORWARD;

	gPedWasMoving = 1;

	return JER_RESULT_CONTINUE;
}

/* JER_EVENT_PED_MOVE — fired inside AnimatePed() right before the player
 * ped's position advances (the ONE write that survives the runner's
 * per-frame MAXRUNSPEED re-arm). The left-stick deflection magnitude sets
 * a target run speed (0 at the deadzone .. PED_MAX_SPEED at the edge) and
 * a short lerp interpolates toward it — Tanner presses off from a
 * stand-start and eases through sharp turns instead of snapping to the
 * fixed run speed. */
int D2plOnPedMove(void* userdata, void* args)
{
	JER_ARGS_PED_MOVE* a = (JER_ARGS_PED_MOVE*)args;
	LPPEDESTRIAN pPed = (LPPEDESTRIAN)a->ped;
	int m;
	int target = 0;

	(void)userdata;

	if (pPed == NULL)
		return JER_RESULT_CONTINUE;

	if (gS.cameraEnabled == 0)
		return JER_RESULT_CONTINUE;

	/* the dominant axis: a straight push to the edge is full speed
	 * (the sum would only reach full speed on a diagonal) */
	{
		int mx = ABS(Pads[a->padId].mapanalog[2]);
		int my = ABS(Pads[a->padId].mapanalog[3]);

		m = (mx > my) ? mx : my;
	}

	if (m > MOVE_DEADZONE)
	{
		target = (m - MOVE_DEADZONE) * PED_MAX_SPEED
			/ (127 - MOVE_DEADZONE);

		if (target > PED_MAX_SPEED)
			target = PED_MAX_SPEED;
	}

	if (gPedStickMove)
	{
		/* the stick drives the movement: lerp the momentum toward the
		 * target (snap once the gap is under one step so the integer
		 * truncation can't stall the decay above zero — the creep) and
		 * write it as the run speed */
		int delta = target - gPedRunSpeed;

		if (ABS(delta) < PED_SPEED_LERP)
			gPedRunSpeed = target;
		else
			gPedRunSpeed += delta / PED_SPEED_LERP;

		pPed->speed = gPedRunSpeed;
	}
	else if (pPed->speed == 0)
	{
		/* fully stopped: bleed the momentum off so the next press-off
		 * starts from a stand. While the engine is still moving him
		 * (D-pad run/walk-back), the momentum is FROZEN — no mid-run
		 * slow-start hitch when the stick re-engages. */
		gPedRunSpeed -= gPedRunSpeed / 8;

		if (gPedRunSpeed < 1)
			gPedRunSpeed = 0;
	}
	/* else: the stick is centered — leave the engine's value (the runner
	 * 40, the walk-back -10, or 0) so D-pad/keyboard movement still works */

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

			/* subtle view bob/sway: a gentle bounce while Tanner runs
			 * (the steps), and a tiny breathing sway while he stands —
			 * both should be barely perceptible */
			{
				int speed = ABS(lp->pos[0] - gBobLastX) + ABS(lp->pos[2] - gBobLastZ);

				gBobLastX = lp->pos[0];
				gBobLastZ = lp->pos[2];

				if (speed > 8)
				{
					camPos->vy += FIXEDH(RSIN(FrameCnt * 3) * 10);
					camPos->vx += FIXEDH(RCOS(FrameCnt * 2) * 8);
				}
				else
				{
					/* natural standing sway (idle breathing) */
					camPos->vx += FIXEDH(RSIN(FrameCnt) * 3);
					camPos->vy += FIXEDH(RCOS(FrameCnt >> 1) * 4);
				}
		}
	}
}
