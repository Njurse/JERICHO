/*
 * d2pl_car.c — Driver 2 Parallel Lines (in-car chase camera, velocity-blend natural heading, instability governor).
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

/* --- car-related shared state (defined here; extern in d2pl.h) --- */

int gVelAngle;		/* smoothed ground-velocity heading (0..4095) */
int gSpeedSmooth;	/* smoothed wheel speed (FIXEDH units, 0 on foot) */
int gLastBaseX, gLastBaseZ;
int gAngRate;		/* smoothed car-body angular rate (units/frame) */
int gLastCarDir;		/* previous car heading for the rate delta */
int gInstability;	/* 1 = the Instability Governor holds the camera */
int gInstabConfirm;	/* frames the rate has stayed calm (hysteresis) */
int gTrackedCarId = -2;	/* the car the governor is sampling (seeds the
				   angular-rate delta on car entry, so the first
				   in-car frame can't false-trigger a spin) */

/* ================================================================== */
/* D2plCarSpeed - smoothed wheel speed for the pull / FOV / height        */
/* ================================================================== */

int D2plCarSpeed(PLAYER* lp)
{
	if (lp->playerCarId < 0)
		return 0;

	return ABS(FIXEDH(car_data[lp->playerCarId].hd.wheel_speed));
}

/* scr_z that yields `degrees` of horizontal FOV at the current aspect
 * ratio, inverting the engine's FrAng = ratan2(160, scr_z * aspect * 1.35) */

/* ================================================================== */
/* D2plNaturalHeading - the live natural-settle target                    */
/* ================================================================== */

int D2plNaturalHeading(PLAYER* lp, int inCar)
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


/* ================================================================== */
/* D2plTrackInstability - the Instability Governor (smoothed body         */
/* angular rate + hysteresis: holds the camera during spins/crashes)      */
/* ================================================================== */

void D2plTrackInstability(PLAYER* lp, int inCar)
{
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

}

/* ================================================================== */
/* D2plCarCamera - GTA4-style chase framing for vehicles                   */
/* ================================================================== */

void D2plCarCamera(void* args, int heading)
{
	JER_ARGS_CAMERA* a = (JER_ARGS_CAMERA*)args;
	PLAYER* lp = (PLAYER*)a->player;
	VECTOR* camPos = (VECTOR*)a->cameraPosition;

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
		camPos->vy = -base[1] + gS.carHeight - (gSpeedSmooth * HEIGHT_SPEED_SCALE) / 4096;

		camPos->vx += FIXEDH(RCOS(heading) * lateral * gS.shoulder);
		camPos->vz += -FIXEDH(RSIN(heading) * lateral * gS.shoulder);
}
