/*
 * jerai_behavior.c -- the behavior table and its implementations.
 *
 * Joyride is the first real behavior: it picks a random road sample somewhere in
 * the window as a destination, plans there with A*, and steers along the plan -
 * so the car drives around and takes the streets. The map's road preference and
 * the pad's traffic/wall handling do the "avoid" part.
 */

#include "jerai_internal.h"
#include "jerai_behavior.h"
#include "jerai_drive.h"

#include "dr2math.h"	/* ratan2, RSIN, RCOS */

#include <math.h>

/* ------------------------------------------------------------------ */
/* Joyride                                                             */
/* ------------------------------------------------------------------ */

/* Pick a goal: a random ROAD sample in the window, far enough away to be a
 * drive and near enough to be one plan's worth, AND not behind the car - a
 * destination the car would have to turn around for is exactly the "it keeps
 * U-turning" complaint. Reservoir-sampled, so every eligible road sample has an
 * equal chance. Returns 1 and writes world coords. */
#define JERAI_JOY_MIN_RING	6	/* samples (~3,000 world units) */
#define JERAI_JOY_MAX_RING	12	/* samples (~6,000); must stay well inside the
					 * window (radius 24) or the goal falls out of
					 * it on a re-plan and A* cannot plan at all */

/* a candidate this far behind the car's heading is skipped (cos > -0.35 ~= within
 * 110 deg: straight ahead, a turn away, and a little past perpendicular all pass) */
#define JERAI_JOY_BEHIND_COS	(-1450)

static int JerAiJoyPickGoal(CAR_DATA* car, int* gx, int* gz)
{
	int cx, cz, ix, iz;
	int candX = 0, candZ = 0, count = 0;
	int cwX = car->hd.where.t[0];
	int cwZ = car->hd.where.t[2];
	int fwdX = RSIN(car->hd.direction);
	int fwdZ = RCOS(car->hd.direction);

	if (!JerAiMapValid(&gJerAi.map))
		return 0;

	if (!JerAiMapSampleIndex(&gJerAi.map, gJerAi.map.centreX, gJerAi.map.centreZ, &cx, &cz))
		return 0;

	for (iz = 0; iz < JERAI_MAP_SIZE; iz++)
	{
		for (ix = 0; ix < JERAI_MAP_SIZE; ix++)
		{
			int dx, dz, r, wx, wz;
			long rx, rz, rlen2, cosScaled;

			if (!JerAiMapRoad(&gJerAi.map, ix, iz))
				continue;

			dx = ix - cx;
			dz = iz - cz;
			if (dx < 0) dx = -dx;
			if (dz < 0) dz = -dz;

			r = (dx > dz) ? dx : dz;

			if (r < JERAI_JOY_MIN_RING || r > JERAI_JOY_MAX_RING)
				continue;

			JerAiMapWorld(&gJerAi.map, ix, iz, &wx, &wz);

			rx = wx - cwX;
			rz = wz - cwZ;
			rlen2 = rx * rx + rz * rz;

			/* the directional guide: do not pick a destination behind us */
			cosScaled = (rlen2 > 0)
				? ((long)fwdX * rx + (long)fwdZ * rz) / (long)(sqrt((double)rlen2) + 1.0)
				: 4096;

			if (cosScaled < JERAI_JOY_BEHIND_COS)
				continue;

			count++;

			if (JerAiRand() % count == 0)
			{
				candX = wx;
				candZ = wz;
			}
		}
	}

	if (count == 0)
		return 0;

	*gx = candX;
	*gz = candZ;

	return 1;
}

static int sGhostX, sGhostZ;	/* the current joyride destination, in world units */
static int sHasGoal;
static int sLastPickTick;	/* throttle: a re-pick costs a grid build */

static int JerAiJoyArrived(CAR_DATA* car)
{
	int dx = sGhostX - car->hd.where.t[0];
	int dz = sGhostZ - car->hd.where.t[2];

	return (long)dx * dx + (long)dz * dz < 1600L * 1600L;
}

static unsigned int JerAiJoyride(CAR_DATA* car)
{
	int aimX, aimZ;
	int force = 0;

	/* A fresh destination when we have none, have arrived, when the plan could
	 * not be made at all, when A* could not REACH the current goal, or when the
	 * stuck recovery asks for one. The last three are the important ones: a goal
	 * that has drifted out of the grid window makes A* refuse, and a goal across
	 * a barrier leaves a partial path the car never completes - without re-picking
	 * it would sit there forever ("glued"). */
	if ((!sHasGoal || JerAiJoyArrived(car) || !gJerAi.havePlan || gJerAi.incomplete || gJerAi.forceNewGoal) &&
	    gJerAi.tick - sLastPickTick >= 10)
	{
		sLastPickTick = gJerAi.tick;
		JerAiMapBuild(&gJerAi.map, car->hd.where.t[0], car->hd.where.t[1], car->hd.where.t[2]);
		gJerAi.forceNewGoal = 0;

		if (JerAiJoyPickGoal(car, &sGhostX, &sGhostZ))
		{
			sHasGoal = 1;
			force = 1;
		}
		else
		{
			/* no road in the window (e.g. off-map): fall back to the local search
			 * so the car heads back toward a road rather than idling */
			JERAI_GOAL goal;

			sHasGoal = 0;

			if (JerAiLocalGoal(&gJerAi.map, car->hd.where.t[0], car->hd.where.t[2], &goal))
			{
				sGhostX = goal.x;
				sGhostZ = goal.z;
				sHasGoal = 1;
				force = 1;
			}
		}
	}

	if (!JerAiDrivePlanTo(car, sGhostX, sGhostZ, force))
		return 0;	/* no plan: coast, and re-pick the goal next frame */

	if (!JerAiDriveAim(&aimX, &aimZ))
		return 0;

	gJerAi.aimX = aimX;
	gJerAi.aimZ = aimZ;

	return JerAiDriveSteerPad(car, aimX, aimZ);
}

/* ------------------------------------------------------------------ */
/* the table                                                            */
/* ------------------------------------------------------------------ */

/* index 0 is Manual (no tick); the rest are the drive behaviors */
static const JERAI_BEHAVIOR gJerAiBehaviors[] =
{
	{ JERAI_MODE_MANUAL,  "Manual",  NULL },
	{ JERAI_MODE_JOYRIDE, "Joyride", JerAiJoyride },
};

#define JERAI_BEHAVIOR_COUNT ((int)(sizeof(gJerAiBehaviors) / sizeof(gJerAiBehaviors[0])))

const JERAI_BEHAVIOR* JerAiBehaviorFor(int mode)
{
	int i;

	for (i = 0; i < JERAI_BEHAVIOR_COUNT; i++)
	{
		if (gJerAiBehaviors[i].id == mode && gJerAiBehaviors[i].tick != NULL)
			return &gJerAiBehaviors[i];
	}

	return NULL;
}

int JerAiBehaviorCount(void)
{
	return JERAI_BEHAVIOR_COUNT;
}

int JerAiBehaviorIdAt(int index)
{
	if (index < 0 || index >= JERAI_BEHAVIOR_COUNT)
		return JERAI_MODE_MANUAL;

	return gJerAiBehaviors[index].id;
}

const char* JerAiBehaviorNameAt(int index)
{
	if (index < 0 || index >= JERAI_BEHAVIOR_COUNT)
		return "?";

	return gJerAiBehaviors[index].name;
}

const char* JerAiBehaviorName(int mode)
{
	int i;

	for (i = 0; i < JERAI_BEHAVIOR_COUNT; i++)
	{
		if (gJerAiBehaviors[i].id == mode)
			return gJerAiBehaviors[i].name;
	}

	return "Manual";
}
