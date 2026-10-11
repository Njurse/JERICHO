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

#include "pad.h"	/* CAR_PAD_ACCEL / CAR_PAD_BRAKE, for the follow clearance hold */

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
#define JERAI_JOY_MIN_RING	12	/* samples (~6,000 world units) */
#define JERAI_JOY_MAX_RING	20	/* samples (~10,000); must stay well inside the
					 * window (radius 24) or the goal falls out of
					 * it on a re-plan and A* cannot plan at all */

/* a candidate this far behind the car's heading is skipped (cos > -0.35 ~= within
 * 110 deg: straight ahead, a turn away, and a little past perpendicular all pass) */
#define JERAI_JOY_BEHIND_COS	(-1450)

static int JerAiJoyPickGoal(CAR_DATA* car, int wild, int* gx, int* gz)
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

			if (wild)
			{
				/* a WILD re-pick after a stutter: anywhere in reach (the whole
				 * window), so the car leaves whatever wedged it instead of niggling
				 * at the same spot - and with no directional gate, either */
				if (r < JERAI_JOY_MIN_RING)
					continue;
			}
			else if (r < JERAI_JOY_MIN_RING || r > JERAI_JOY_MAX_RING)
			{
				continue;
			}

			JerAiMapWorld(&gJerAi.map, ix, iz, &wx, &wz);

			rx = wx - cwX;
			rz = wz - cwZ;
			rlen2 = rx * rx + rz * rz;

			if (!wild)
			{
				/* the directional guide: do not pick a destination behind us */
				cosScaled = (rlen2 > 0)
					? ((long)fwdX * rx + (long)fwdZ * rz) / (long)(sqrt((double)rlen2) + 1.0)
					: 4096;

				if (cosScaled < JERAI_JOY_BEHIND_COS)
					continue;
			}

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

/* The A* window is JERAI_MAP_SIZE samples of JERAI_MAP_STEP units, centred on
 * the car - so a goal further than ~12,000 world units cannot even be SAMPLED,
 * and JerAiStarPlan refuses outright (no waypoints at all). A chase target is
 * routinely further than that: the host drives off and is gone. Clamp the PLAN
 * goal to the window edge in the target's direction, so the car drives as far
 * toward them as it can plan; every re-plan recentres the window further along,
 * so it keeps closing the gap. The REAL distance (below) still drives the
 * clearance hold and the log, so the clamp changes nothing about the hold. */
static void JerAiClampToWindow(CAR_DATA* car, int* x, int* z)
{
	long long dx = (long long)*x - (long long)car->hd.where.t[0];
	long long dz = (long long)*z - (long long)car->hd.where.t[2];
	long long d2 = dx * dx + dz * dz;
	long limit = (long long)(JERAI_MAP_SIZE / 2 - 2) * JERAI_MAP_STEP;
	double d;

	if (d2 <= limit * limit)
		return;

	d = sqrt((double)d2);

	if (d <= 0.0)
		return;

	*x = car->hd.where.t[0] + (int)((double)dx * (double)limit / d);
	*z = car->hd.where.t[2] + (int)((double)dz * (double)limit / d);
}

static unsigned int JerAiJoyride(CAR_DATA* car)
{
	int aimX, aimZ;
	int force = 0;
	int followX = 0, followZ = 0;
	long long followD2 = 0;

	/* MULTIPLAYER. With a session live the destination is the HOST's car, so
	 * the AI car tails the host around the map instead of driving to a random
	 * road sample. JerAiFollowTarget answers 0 outside a session, on the host
	 * machine itself, and when the host has no car - and then the normal wander
	 * below runs unchanged. */
	if (gJerAi.followEnabled && JerAiFollowTarget(car, &followX, &followZ, &followD2))
	{
		long long clearance = (long long)gJerAi.followClearance;
		unsigned int pad;

		/* the once-a-second log reads these; the distance is CLAMPED so a far
		 * target does not wrap on the way into an int */
		gJerAi.followActive = 1;
		gJerAi.followDist = (followD2 > 2000000000LL) ? 2000000000 : (int)followD2;

		/* the pad actually handed over this frame; the early exits below leave it
		 * at 0, so the log never shows a stale pad from an earlier frame */
		gJerAi.lastPad = 0;

		/* CLEARANCE: close enough. Stop and hold instead of driving into the
		 * host - and hold through JerAiDriveHoldPad, not the steering pad, so
		 * the stuck detector in there cannot fire a reverse burst at the car we
		 * are keeping clear of. */
		if (followD2 < clearance * clearance)
			return JerAiDriveHoldPad(car);

		/* The goal IS the host, and the host moves: re-stamp it every frame.
		 * Deliberately NOT the repick block below - that rebuilds the occupancy
		 * grid, and it treats an INCOMPLETE plan (a host standing off the road)
		 * as a reason to abandon the goal. JerAiDrivePlanTo re-plans on its own
		 * once the host has drifted past replan_move. Clamped to the planner's
		 * window, because a host across the city is beyond what A* can sample. */
		JerAiClampToWindow(car, &followX, &followZ);

		sGhostX = followX;
		sGhostZ = followZ;
		sHasGoal = 1;
		gJerAi.forceNewGoal = 0;

		if (!JerAiDrivePlanTo(car, sGhostX, sGhostZ, 0))
			return 0;	/* no plan: coast, and try again next frame */

		if (!JerAiDriveAim(&aimX, &aimZ))
			return 0;

		gJerAi.aimX = aimX;
		gJerAi.aimZ = aimZ;

		pad = JerAiDriveSteerPad(car, aimX, aimZ);

		/* Ease off on the approach: within twice the clearance, do not power
		 * toward the host, so the stop is a coast-and-brake rather than a lunge
		 * (the pad's own close-traffic brake is the hard backstop). */
		if (followD2 < clearance * clearance * 4)
			pad &= ~(unsigned int)CAR_PAD_ACCEL;

		/* lastPad is what the once-a-second log prints, and JerAiDriveSteerPad
		 * set it to the pad BEFORE the ease-off strip - re-stamp it so the line
		 * shows the pad the engine actually received. */
		gJerAi.lastPad = (int)pad;

		return pad;
	}

	gJerAi.followActive = 0;
	gJerAi.followDist = 0;

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

		if (JerAiJoyPickGoal(car, gJerAi.reposition, &sGhostX, &sGhostZ))
		{
			sHasGoal = 1;
			gJerAi.reposition = 0;
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
