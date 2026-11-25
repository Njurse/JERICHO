/*
 * alleycat_behavior.c -- the behavior table and its implementations.
 *
 * Joyride is the first real behavior: it picks a random road sample somewhere in
 * the window as a destination, plans there with A*, and steers along the plan -
 * so the car drives around and takes the streets. The map's road preference and
 * the pad's traffic/wall handling do the "avoid" part.
 */

#include "alleycat_internal.h"
#include "alleycat_behavior.h"
#include "alleycat_drive.h"

#include "pad.h"	/* CAR_PAD_ACCEL / CAR_PAD_BRAKE, for the follow clearance hold */

#include "dr2math.h"	/* ratan2, RSIN, RCOS */
#include "cop_ai.h"	/* copsAreInPursuit - Evade's "they are after us" */
#include "felony.h"	/* CopsCanSeePlayer */

#include <math.h>

/* ------------------------------------------------------------------ */
/* Joyride                                                             */
/* ------------------------------------------------------------------ */

/* Pick a goal: a random ROAD sample in the window, far enough away to be a
 * drive and near enough to be one plan's worth, AND not behind the car - a
 * destination the car would have to turn around for is exactly the "it keeps
 * U-turning" complaint. Reservoir-sampled, so every eligible road sample has an
 * equal chance. Returns 1 and writes world coords. */
#define ALLEYCAT_JOY_MIN_RING	12	/* samples (~6,000 world units) */
#define ALLEYCAT_JOY_MAX_RING	20	/* samples (~10,000); must stay well inside the
					 * window (radius 24) or the goal falls out of
					 * it on a re-plan and A* cannot plan at all */

/* A candidate must lie in a FORWARD cone of the car's heading (cos > +0.34 ~= within
 * 70 deg). The old gate only rejected goals more than 110 deg off, so a pick to
 * the side made the car turn across the road and one just past perpendicular made
 * it swing back the way it came - the "random U-turn" look. Keeping the cone
 * forward means the wander carries on in the direction it is already driving. */
#define ALLEYCAT_JOY_BEHIND_COS	(1400)

static int AlleycatJoyPickGoal(CAR_DATA* car, int wild, int* gx, int* gz)
{
	int cx, cz, ix, iz;
	int candX = 0, candZ = 0, count = 0;
	int cwX = car->hd.where.t[0];
	int cwZ = car->hd.where.t[2];
	int fwdX = RSIN(car->hd.direction);
	int fwdZ = RCOS(car->hd.direction);

	if (!AlleycatMapValid(&gAlleycat.map))
		return 0;

	if (!AlleycatMapSampleIndex(&gAlleycat.map, gAlleycat.map.centreX, gAlleycat.map.centreZ, &cx, &cz))
		return 0;

	for (iz = 0; iz < ALLEYCAT_MAP_SIZE; iz++)
	{
		for (ix = 0; ix < ALLEYCAT_MAP_SIZE; ix++)
		{
			int dx, dz, r, wx, wz;
			long long rx, rz, rlen2, cosScaled;

			if (!AlleycatMapRoad(&gAlleycat.map, ix, iz))
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
				if (r < ALLEYCAT_JOY_MIN_RING)
					continue;
			}
			else if (r < ALLEYCAT_JOY_MIN_RING || r > ALLEYCAT_JOY_MAX_RING)
			{
				continue;
			}

			AlleycatMapWorld(&gAlleycat.map, ix, iz, &wx, &wz);

			rx = wx - cwX;
			rz = wz - cwZ;
			rlen2 = rx * rx + rz * rz;

			if (!wild)
			{
				/* the directional guide: do not pick a destination behind us */
				cosScaled = (rlen2 > 0)
					? ((long long)fwdX * rx + (long long)fwdZ * rz) / (long long)(sqrt((double)rlen2) + 1.0)
					: 4096;

				if (cosScaled < ALLEYCAT_JOY_BEHIND_COS)
					continue;
			}

			count++;

			if (AlleycatRand() % count == 0)
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

static int sGhostX, sGhostZ;	/* the current destination, in world units */
static int sHasGoal;
static int sLastPickTick;	/* throttle: a re-pick costs a grid build */
static int sThreatX, sThreatZ;	/* where the threat was when the bolt-hole was picked */

static int AlleycatJoyArrived(CAR_DATA* car)
{
	/* 64-bit on purpose: a 32-bit squared world distance WRAPS NEGATIVE past
	 * ~46,000 units, so a goal on the far side of the level would read as
	 * "arrived" and the pick would spin, re-choosing a destination every 10
	 * frames while the car merely looked close to it. */
	long long dx = (long long)sGhostX - (long long)car->hd.where.t[0];
	long long dz = (long long)sGhostZ - (long long)car->hd.where.t[2];

	return (dx * dx + dz * dz) < (1600LL * 1600LL);
}

/* The A* window is ALLEYCAT_MAP_SIZE samples of ALLEYCAT_MAP_STEP units, centred on
 * the car - so a goal further than ~12,000 world units cannot even be SAMPLED,
 * and AlleycatStarPlan refuses outright (no waypoints at all). A chase target is
 * routinely further than that: the host drives off and is gone. Clamp the PLAN
 * goal to the window edge in the target's direction, so the car drives as far
 * toward them as it can plan; every re-plan recentres the window further along,
 * so it keeps closing the gap. The REAL distance (below) still drives the
 * clearance hold and the log, so the clamp changes nothing about the hold. */
static void AlleycatClampToWindow(CAR_DATA* car, int* x, int* z)
{
	long long dx = (long long)*x - (long long)car->hd.where.t[0];
	long long dz = (long long)*z - (long long)car->hd.where.t[2];
	long long d2 = dx * dx + dz * dz;
	long long limit = (long long)(ALLEYCAT_MAP_SIZE / 2 - 2) * ALLEYCAT_MAP_STEP;
	double d;

	if (d2 <= limit * limit)
		return;

	d = sqrt((double)d2);

	if (d <= 0.0)
		return;

	*x = car->hd.where.t[0] + (int)((double)dx * (double)limit / d);
	*z = car->hd.where.t[2] + (int)((double)dz * (double)limit / d);
}

/* A fresh bolt-hole when we have none, have arrived, the plan failed, the stuck
 * recovery asks for one, or the threat has moved far enough (`threatMoved`) that
 * "away from it" is now somewhere else. Shared by Joyride's flee and Evade. */
static void AlleycatPickFleeGoal(CAR_DATA* car, int targetX, int targetZ, int threatMoved, int* force)
{
	ALLEYCAT_GOAL goal;

	if (!((!sHasGoal || AlleycatJoyArrived(car) || !gAlleycat.havePlan || gAlleycat.forceNewGoal || threatMoved) &&
	      gAlleycat.tick - sLastPickTick >= 10))
		return;

	sLastPickTick = gAlleycat.tick;
	gAlleycat.forceNewGoal = 0;
	gAlleycat.reposition = 0;	/* a fresh bolt-hole IS the relocation */
	sThreatX = targetX;
	sThreatZ = targetZ;

	/* Fleeing is scored on the SAME grid the plan uses: the goal is a sample the
	 * car can actually REACH that is furthest from the threat, not a straight
	 * line away from it through a building. */
	if (AlleycatDriveBuildMap(car) &&
	    AlleycatLocalFleeGoal(&gAlleycat.map, car->hd.where.t[0], car->hd.where.t[2], targetX, targetZ, &goal))
	{
		sGhostX = goal.x;
		sGhostZ = goal.z;
		sHasGoal = 1;
		*force = 1;
	}
	else
	{
		/* nowhere to bolt to: get back to a road and try again next frame */
		sHasGoal = 0;

		if (AlleycatLocalGoal(&gAlleycat.map, car->hd.where.t[0], car->hd.where.t[2], &goal))
		{
			sGhostX = goal.x;
			sGhostZ = goal.z;
			sHasGoal = 1;
			*force = 1;
		}
	}
}

/* A fresh wander destination when we have none, have arrived, when the plan could
 * not be made at all, when A* could not REACH the current goal, or when the stuck
 * recovery asks for one. The last three are the important ones: a goal that has
 * drifted out of the grid window makes A* refuse, and a goal across a barrier
 * leaves a partial path the car never completes - without re-picking it would sit
 * there forever ("glued"). Shared by Joyride and Evade. */
static void AlleycatPickWanderGoal(CAR_DATA* car, int* force)
{
	if (!((!sHasGoal || AlleycatJoyArrived(car) || !gAlleycat.havePlan || gAlleycat.incomplete || gAlleycat.forceNewGoal) &&
	      gAlleycat.tick - sLastPickTick >= 10))
		return;

	sLastPickTick = gAlleycat.tick;
	AlleycatDriveBuildMap(car);
	gAlleycat.forceNewGoal = 0;

	if (AlleycatJoyPickGoal(car, gAlleycat.reposition, &sGhostX, &sGhostZ))
	{
		sHasGoal = 1;
		gAlleycat.reposition = 0;
		*force = 1;
	}
	else
	{
		/* no road in the window (e.g. off-map): fall back to the local search so
		 * the car heads back toward a road rather than idling */
		ALLEYCAT_GOAL goal;

		sHasGoal = 0;

		if (AlleycatLocalGoal(&gAlleycat.map, car->hd.where.t[0], car->hd.where.t[2], &goal))
		{
			sGhostX = goal.x;
			sGhostZ = goal.z;
			sHasGoal = 1;
			*force = 1;
		}
	}
}

/* The shared wander/chase body. `pursue` switches on the player-follow target
 * (the Pursuit behavior); Joyride leaves it off and just drives around. */
static unsigned int AlleycatDrive(CAR_DATA* car, int pursue)
{
	int aimX, aimZ;
	int force = 0;
	int targetX = 0, targetZ = 0;
	long long followD2 = 0;
	int following = 0;
	unsigned int pad;

	gAlleycat.fleeActive = 0;
	gAlleycat.policeActive = 0;
	gAlleycat.panicFlee = 0;
	gAlleycat.followCarId = -1;	/* the resolvers below set it when there IS one */
	gAlleycat.speedCap = 0;		/* Evade's caps are Evade's alone */
	gAlleycat.steerCap = 0;
	gAlleycat.evadeActive = 0;
	gAlleycat.lightHold = 0;

	/* The destination, in priority order:
	 *
	 *   1. POLICE - the cops are after us: drop everything, follow included, and run
	 *               from the nearest police car FLAT OUT. Whatever was otherwise
	 *               happening resumes the moment the pursuit ends;
	 *   2. FLEE   - in a session, the HOST runs from the nearest other player, so
	 *               the cars that joined to follow have something to chase;
	 *   3. FOLLOW - PURSUIT only: in a session, a CLIENT drives to the host's car;
	 *   4. wander - a random road sample (single player, a session with nobody
	 *               else in it, or Joyride, which never follows).
	 *
	 * Each resolver answers 0 when its condition does not apply, so exactly one
	 * branch runs and the wander below stays the plain single-player behaviour.
	 *
	 * The police and the host-flee SHARE one body: both want "the furthest
	 * reachable point from a threat" and differ only in who the threat is and in
	 * whether the pace limit is lifted. */
	if (gAlleycat.fleePolice && AlleycatPoliceThreat(car, &targetX, &targetZ))
		gAlleycat.policeActive = 1;	/* panicFlee below lifts the pace limit */

	if (gAlleycat.policeActive || (gAlleycat.fleeEnabled && AlleycatThreatTarget(car, &targetX, &targetZ))
		|| (gAlleycat.fleeHost && AlleycatHostThreatTarget(car, &targetX, &targetZ)))
	{
		long long mdx = (long long)targetX - sThreatX;
		long long mdz = (long long)targetZ - sThreatZ;
		long long move = (long long)gAlleycat.replanMove;

		gAlleycat.fleeActive = 1;
		gAlleycat.tanActive = 0;	/* the follow detour is not for fleeing */
		gAlleycat.panicFlee = gAlleycat.policeActive;	/* police = flat out, no pace limit */

		/* A CLOSING pursuer covers replan_move almost instantly, so the plain
		 * "the threat moved" trigger redrew the bolt-hole every ~12 frames - and
		 * a destination that is redrawn 2.5 times a second is never DRIVEN TO:
		 * the aim swings, the heading error stays huge and the car looks lazy
		 * while it thrashes. A fleeing car COMMITS to a direction, so the police
		 * case draws a much longer line before reconsidering. */
		if (gAlleycat.policeActive)
			move *= 6;

		AlleycatPickFleeGoal(car, targetX, targetZ, (mdx * mdx + mdz * mdz) > move * move, &force);
	}
	else if (pursue && gAlleycat.followEnabled && AlleycatFollowTarget(car, &targetX, &targetZ, &followD2))
	{
		long long clearance = (long long)gAlleycat.followClearance;

		following = 1;

		int jink = 0;

		/* the once-a-second log reads these; the distance is CLAMPED so a far
		 * target does not wrap on the way into an int */
		gAlleycat.followActive = 1;
		gAlleycat.followDist = (followD2 > 2000000000LL) ? 2000000000 : (int)followD2;

		/* the pad actually handed over this frame; the early exit below leaves it
		 * at 0, so the log never shows a stale pad from an earlier frame */
		gAlleycat.lastPad = 0;

		/* CLEARANCE: close enough. Stop and hold instead of driving into the
		 * host - and hold through AlleycatDriveHoldPad, not the steering pad, so
		 * the stuck detector in there cannot fire a reverse burst at the car we
		 * are keeping clear of. In RAM mode there is no clearance: the whole
		 * point is to drive into it. */
		if (!gAlleycat.pursuitRam && followD2 < clearance * clearance)
			return AlleycatDriveHoldPad(car);

		/* JINK. The stuck recovery has just backed the car off something and
		 * asked for a different destination. Re-stamping the host here would
		 * send it straight back through the same wedge, and the loop would never
		 * break - so for ONE pick, take the recovery's advice and aim at the
		 * nearest reachable open ground instead. The next frame follows the host
		 * again from wherever that leaves the car. */
		if (gAlleycat.forceNewGoal)
		{
			ALLEYCAT_GOAL escape;

			gAlleycat.forceNewGoal = 0;
			gAlleycat.reposition = 0;

			/* TANGENTIAL DETOUR. The stuck recovery just backed the car off
			 * something while following, so re-stamping the host would drive it
			 * straight back into the same wedge and the loop never breaks. Instead
			 * pick a spot SIDEWAYS to the car->host line -- perpendicular to it, so
			 * the wedge is broken rather than rejoined -- and drive TO it before
			 * re-routing. The old jink used the nearest reachable open ground,
			 * which can be back through the very gap the car is wedged in. */
			if (AlleycatDriveBuildMap(car) &&
			    AlleycatLocalTangentialGoal(&gAlleycat.map, car->hd.where.t[0], car->hd.where.t[2],
				    targetX, targetZ, &escape))
			{
				gAlleycat.tanX = escape.x;
				gAlleycat.tanZ = escape.z;
				gAlleycat.tanActive = 1;

				sGhostX = escape.x;
				sGhostZ = escape.z;
				sHasGoal = 1;
				jink = 1;
			}
		}

		/* HOLD the tangential waypoint until reached, then re-route to the host.
		 * This is the "drive to a tangentially related spot FIRST" half: without
		 * it the detour lasts one frame and the car is back to head-butting the
		 * same obstacle. */
		if (gAlleycat.tanActive && !jink)
		{
			if (AlleycatJoyArrived(car))
				gAlleycat.tanActive = 0;	/* next frame follows the host again */
			else
			{
				sGhostX = gAlleycat.tanX;
				sGhostZ = gAlleycat.tanZ;
				sHasGoal = 1;
				jink = 1;
			}
		}

		if (!jink)
		{
			/* The goal IS the host, and the host moves: re-stamp it every frame.
			 * Deliberately NOT the repick block below - that rebuilds the
			 * occupancy grid, and it treats an INCOMPLETE plan (a host standing
			 * off the road) as a reason to abandon the goal. AlleycatDrivePlanTo
			 * re-plans on its own once the host has drifted past replan_move.
			 * Clamped to the planner's window, because a host across the city is
			 * beyond what A* can sample. */
			AlleycatClampToWindow(car, &targetX, &targetZ);

			sGhostX = targetX;
			sGhostZ = targetZ;
			sHasGoal = 1;
			gAlleycat.forceNewGoal = 0;
		}
	}
	else
	{
		gAlleycat.followActive = 0;
		gAlleycat.followDist = 0;
		gAlleycat.tanActive = 0;	/* no host, no detour to it */

		/* A fresh destination when we have none, have arrived, when the plan could
		 * not be made at all, when A* could not REACH the current goal, or when the
		 * stuck recovery asks for one. The last three are the important ones: a goal
		 * that has drifted out of the grid window makes A* refuse, and a goal across
		 * a barrier leaves a partial path the car never completes - without re-picking
		 * it would sit there forever ("glued"). */
		AlleycatPickWanderGoal(car, &force);
	}	/* end: the random-wander destination */

	if (!AlleycatDrivePlanTo(car, sGhostX, sGhostZ, force))
		return 0;	/* no plan: coast, and re-pick the goal next frame */

	if (!AlleycatDriveAim(&aimX, &aimZ))
		return 0;

	gAlleycat.aimX = aimX;
	gAlleycat.aimZ = aimZ;

	pad = AlleycatDriveSteerPad(car, aimX, aimZ);

	if (following)
	{
		long long clearance = (long long)gAlleycat.followClearance;

		/* Ease off on the approach: within twice the clearance, do not power
		 * toward the host, so the stop is a coast-and-brake rather than a lunge
		 * (the pad's own close-traffic brake is the hard backstop). RAM mode
		 * keeps the throttle planted all the way in. */
		if (!gAlleycat.pursuitRam && followD2 < clearance * clearance * 4)
			pad &= ~(unsigned int)CAR_PAD_ACCEL;

		/* lastPad is what the once-a-second log prints, and AlleycatDriveSteerPad set
		 * it to the pad BEFORE that strip - re-stamp it so the line shows the pad
		 * the engine actually received. */
		gAlleycat.lastPad = (int)pad;
	}

	return pad;
}

/* ------------------------------------------------------------------ */
/* Evade                                                                */
/* ------------------------------------------------------------------ */

/* Evade: drive around like a careful getaway driver. The settings it is built
 * from (Alleycat.ini, see alleycat.h):
 *
 *   cop_clearance  a police car (pursuer, patrolling cop, MP cop player) inside
 *                  this many world units is a threat: leave for the reachable
 *                  point furthest from it (the same bolt-hole the Joyride flee
 *                  uses), and go back to wandering once it is a quarter further
 *                  off again (hysteresis, so the edge does not flicker);
 *   pace_pct       the cruise pace: the road's own limit (`roadLimit`, re-read
 *                  under the car from civ_ai.c speedLimits[]) plus this percent.
 *                  Held even while keeping clear of a cop that is NOT chasing -
 *                  speeding past one is how a pursuit starts;
 *   run_lights     1 = never stop at lights. 0 = stop for a red (or an amber
 *                  that is still far enough off) the way the engine's red-light
 *                  felony reads it, again unless already chased;
 *   max_steer      a wheel-angle cap on the steering, for a smoother line
 *                  (352 or more = none: that is the engine's regular lock);
 *   top_speed      the overall cap, as for every behavior.
 *
 * Once the police are actually after the car (a pursuit is running or one has
 * line of sight) it is the Joyride police flee: flat out, no pace, no lights. */
#define ALLEYCAT_EVADE_LIGHT_RANGE	1800	/* how far ahead a light is looked for */
#define ALLEYCAT_EVADE_LIGHT_STOP	900	/* inside this, brake to a stop; outside, coast */

static int sEvadeHot;	/* last frame a cop was inside the clearance (the release edge) */

static unsigned int AlleycatEvade(CAR_DATA* car)
{
	int targetX = 0, targetZ = 0, aimX, aimZ, lightD = 0;
	int force = 0, threat, pursued, radius;
	long long copD2 = -1;
	unsigned int pad;

	gAlleycat.followActive = 0;
	gAlleycat.followDist = 0;
	gAlleycat.tanActive = 0;
	gAlleycat.fleeActive = 0;
	gAlleycat.lightHold = 0;
	gAlleycat.followCarId = -1;

	/* the road's own limit under the car, and the pace over it */
	gAlleycat.roadLimit = AlleycatDriveRoadLimit(car, gAlleycat.roadLimit);

	pursued = (copsAreInPursuit || CopsCanSeePlayer) ? 1 : 0;

	radius = gAlleycat.copClearance;

	if (sEvadeHot)
		radius += radius / 4;

	threat = AlleycatCopNear(car, radius, &targetX, &targetZ, &copD2);

	/* chased by a pursuer outside the clearance still counts: run */
	if (!threat && pursued)
		threat = AlleycatPoliceThreat(car, &targetX, &targetZ);

	sEvadeHot = threat;
	gAlleycat.evadeActive = threat;
	{
		double d = (copD2 < 0) ? 0.0 : sqrt((double)copD2);

		gAlleycat.evadeCopDist = (d > 1.0e9) ? 1000000000 : (int)d;	/* the readout only */
	}
	gAlleycat.policeActive = pursued;
	gAlleycat.panicFlee = (threat && pursued) ? 1 : 0;

	gAlleycat.speedCap = gAlleycat.panicFlee ? 0 : gAlleycat.roadLimit * (100 + gAlleycat.pacePct) / 100;
	gAlleycat.steerCap = gAlleycat.maxSteer;

	if (gAlleycat.speedCap < 8 && !gAlleycat.panicFlee)
		gAlleycat.speedCap = 8;	/* a silly pace_pct must not park the car */

	if (threat)
	{
		long long mdx = (long long)targetX - sThreatX;
		long long mdz = (long long)targetZ - sThreatZ;
		long long move = (long long)gAlleycat.replanMove;

		gAlleycat.fleeActive = 1;

		if (gAlleycat.panicFlee)
			move *= 6;	/* see Joyride: a chased car commits to a line */

		AlleycatPickFleeGoal(car, targetX, targetZ, (mdx * mdx + mdz * mdz) > move * move, &force);
	}
	else
	{
		AlleycatPickWanderGoal(car, &force);
	}

	if (!AlleycatDrivePlanTo(car, sGhostX, sGhostZ, force))
		return 0;

	if (!AlleycatDriveAim(&aimX, &aimZ))
		return 0;

	gAlleycat.aimX = aimX;
	gAlleycat.aimZ = aimZ;

	/* lights: only while nobody is chasing (a chased car runs them) */
	if (!gAlleycat.runLights && !gAlleycat.panicFlee &&
	    AlleycatDriveStopLightAhead(car, ALLEYCAT_EVADE_LIGHT_RANGE, &lightD))
	{
		gAlleycat.lightHold = 1;

		if (lightD <= ALLEYCAT_EVADE_LIGHT_STOP)
			return AlleycatDriveHoldPad(car);
	}

	pad = AlleycatDriveSteerPad(car, aimX, aimZ);

	if (gAlleycat.lightHold)
	{
		/* a light further off: coast toward it rather than power on */
		pad &= ~(unsigned int)(CAR_PAD_ACCEL | CAR_PAD_WHEELSPIN);
		gAlleycat.lastPad = (int)pad;
	}

	return pad;
}

/* Forget the per-level behavior state (JER_EVENT_GAME_START): the goal and the
 * threat it was picked against are coordinates on the LAST level. */
void AlleycatBehaviorResetRun(void)
{
	sGhostX = sGhostZ = 0;
	sHasGoal = 0;
	sLastPickTick = -1000;
	sThreatX = sThreatZ = 0;
	sEvadeHot = 0;

	gAlleycat.followActive = 0;
	gAlleycat.followDist = 0;
	gAlleycat.fleeActive = 0;
	gAlleycat.policeActive = 0;
	gAlleycat.evadeActive = 0;
	gAlleycat.evadeCopDist = 0;
	gAlleycat.lightHold = 0;
}

static unsigned int AlleycatJoyride(CAR_DATA* car)
{
	return AlleycatDrive(car, 0);
}

static unsigned int AlleycatPursuit(CAR_DATA* car)
{
	return AlleycatDrive(car, 1);
}

/* ------------------------------------------------------------------ */
/* the table                                                            */
/* ------------------------------------------------------------------ */

/* index 0 is Manual (no tick); the rest are the drive behaviors */
static const ALLEYCAT_BEHAVIOR gAlleycatBehaviors[] =
{
	{ ALLEYCAT_MODE_MANUAL,  "Manual",  NULL },
	{ ALLEYCAT_MODE_JOYRIDE, "Joyride", AlleycatJoyride },
	{ ALLEYCAT_MODE_PURSUIT, "Pursuit", AlleycatPursuit },
	{ ALLEYCAT_MODE_EVADE,   "Evade",   AlleycatEvade },
};

#define ALLEYCAT_BEHAVIOR_COUNT ((int)(sizeof(gAlleycatBehaviors) / sizeof(gAlleycatBehaviors[0])))

const ALLEYCAT_BEHAVIOR* AlleycatBehaviorFor(int mode)
{
	int i;

	for (i = 0; i < ALLEYCAT_BEHAVIOR_COUNT; i++)
	{
		if (gAlleycatBehaviors[i].id == mode && gAlleycatBehaviors[i].tick != NULL)
			return &gAlleycatBehaviors[i];
	}

	return NULL;
}

int AlleycatBehaviorCount(void)
{
	return ALLEYCAT_BEHAVIOR_COUNT;
}

int AlleycatBehaviorIdAt(int index)
{
	if (index < 0 || index >= ALLEYCAT_BEHAVIOR_COUNT)
		return ALLEYCAT_MODE_MANUAL;

	return gAlleycatBehaviors[index].id;
}

const char* AlleycatBehaviorNameAt(int index)
{
	if (index < 0 || index >= ALLEYCAT_BEHAVIOR_COUNT)
		return "?";

	return gAlleycatBehaviors[index].name;
}

const char* AlleycatBehaviorName(int mode)
{
	int i;

	for (i = 0; i < ALLEYCAT_BEHAVIOR_COUNT; i++)
	{
		if (gAlleycatBehaviors[i].id == mode)
			return gAlleycatBehaviors[i].name;
	}

	return "Manual";
}
