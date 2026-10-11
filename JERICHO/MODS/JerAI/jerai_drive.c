/*
 * jerai_drive.c -- the driving primitives: plan, aim, pad. See jerai_drive.h.
 *
 * The car is driven by a synthetic pad handed to the engine through
 * JER_EVENT_CAR_PAD, exactly as the mp bot drives (mp_bot.c MpBotSteerToPoint):
 * the car stays CONTROL_TYPE_PLAYER and the engine's own input path turns the
 * pad into steering and throttle.
 *
 * CAR_PAD_HANDBRAKE is deliberately never emitted: handling.c cycles
 * g_PlayerControlMode on a handbrake press edge, which would hand the car to the
 * engine's AI.
 */

#include "jerai_internal.h"
#include "jerai_drive.h"

#include "pad.h"	/* CAR_PAD_* */
#include "players.h"
#include "cars.h"
#include "dr2math.h"	/* ratan2, RSIN, RCOS */
#include "objcoll.h"	/* CellEmpty: the engine's own scenery test, for the feelers */
#include "dr2roads.h"	/* MapHeight */

#include <math.h>

/* ------------------------------------------------------------------ */
/* the run RNG                                                          */
/* ------------------------------------------------------------------ */

static unsigned int sRng = 0x12345678u;

void JerAiRandSeed(unsigned int s)
{
	sRng = s ? s : 0x12345678u;
}

unsigned int JerAiRand(void)
{
	sRng = sRng * 1664525u + 1013904223u;
	return sRng >> 16;
}

/* ------------------------------------------------------------------ */
/* the car                                                              */
/* ------------------------------------------------------------------ */

CAR_DATA* JerAiOwnCar(void)
{
	int id = player[0].playerCarId;

	if (id < 0 || id >= MAX_CARS)
		return NULL;

	return &car_data[id];
}

/* ------------------------------------------------------------------ */
/* planning                                                             */
/* ------------------------------------------------------------------ */

static int JerAiAbs(int v)
{
	return v < 0 ? -v : v;
}

int JerAiDrivePlanTo(CAR_DATA* car, int toX, int toZ, int force)
{
	int cx, cy, cz, age, stale;

	if (car == NULL)
		return 0;

	cx = car->hd.where.t[0];
	cy = car->hd.where.t[1];
	cz = car->hd.where.t[2];

	age = gJerAi.tick - gJerAi.plannedAt;

	stale = force || !gJerAi.havePlan ||
		age >= (gJerAi.replanMs * 30) / 1000 ||
		JerAiAbs(toX - gJerAi.goalX) >= gJerAi.replanMove ||
		JerAiAbs(toZ - gJerAi.goalZ) >= gJerAi.replanMove;

	if (!stale)
		return 1;

	/* Rebuild the window around the car and re-plan. A build that fails means
	 * the world is not there (frontend, between levels): keep any plan we had. */
	if (!JerAiMapBuild(&gJerAi.map, cx, cy, cz))
		return gJerAi.havePlan;

	gJerAi.incomplete = !JerAiStarPlan(&gJerAi.map, cx, cz, toX, toZ, &gJerAi.path);
	gJerAi.plannedAt = gJerAi.tick;
	gJerAi.goalX = toX;
	gJerAi.goalZ = toZ;
	gJerAi.havePlan = (gJerAi.path.waypoints > 0);

	return gJerAi.havePlan;
}

static long JerAiSegLen(long ax, long az, long bx, long bz)
{
	double dx = (double)(bx - ax);
	double dz = (double)(bz - az);

	return (long)sqrt(dx * dx + dz * dz);
}

/* cos(angle) fixed-point: a point is "not behind" the car when the cosine
 * between the car's heading and the direction to the point exceeds this
 * (-0.35 ~= 110 deg, so a perpendicular intersection turn still counts). */
#define JERAI_AIM_BEHIND_COS	(-1450)

int JerAiDriveAim(int* aimX, int* aimZ)
{
	CAR_DATA* car = JerAiOwnCar();
	int n, k;
	long cx, cz;
	long bestD2 = -1, bestArc = 0, arc;
	int dir, fwdX, fwdZ;
	long bestX, bestZ;

	if (car == NULL || !gJerAi.havePlan || gJerAi.path.waypoints <= 0)
		return 0;

	n = gJerAi.path.waypoints;
	cx = car->hd.where.t[0];
	cz = car->hd.where.t[2];
	dir = car->hd.direction;
	fwdX = RSIN(dir);
	fwdZ = RCOS(dir);

	/* debug override: a fixed aim heading, to test the steering in isolation */
	if (gJerAi.aimTest >= 0)
	{
		int d = gJerAi.aimTest & 0xfff;

		gJerAi.aimX = (int)(cx + 4000L * RSIN(d) / 4096);
		gJerAi.aimZ = (int)(cz + 4000L * RCOS(d) / 4096);

		if (aimX) *aimX = gJerAi.aimX;
		if (aimZ) *aimZ = gJerAi.aimZ;

		return 1;
	}

	/* where the car is ALONG the plan: the closest point on the polyline */
	arc = 0;

	for (k = 1; k < n; k++)
	{
		long ax = gJerAi.path.wx[k - 1], az = gJerAi.path.wz[k - 1];
		long bx = gJerAi.path.wx[k], bz = gJerAi.path.wz[k];
		long dx = bx - ax, dz = bz - az;
		long len2 = dx * dx + dz * dz;
		long segLen = JerAiSegLen(ax, az, bx, bz);
		long t, px, pz, d2;

		if (len2 > 0)
		{
			t = (cx - ax) * dx + (cz - az) * dz;

			if (t < 0) t = 0;
			if (t > len2) t = len2;
		}
		else
		{
			t = 0;
		}

		px = ax + (len2 > 0 ? dx * t / len2 : 0);
		pz = az + (len2 > 0 ? dz * t / len2 : 0);
		d2 = (cx - px) * (cx - px) + (cz - pz) * (cz - pz);

		if (bestD2 < 0 || d2 < bestD2)
		{
			bestD2 = d2;
			bestArc = arc + (len2 > 0 ? segLen * t / len2 : 0);
		}

		arc += segLen;
	}

	/* Walk FORWARD from there and take the first point that is both at least
	 * look_ahead further along AND not behind the car (the DIRECTIONAL GUIDE).
	 * Skimming the segment in ~512-unit steps means a point behind the car is
	 * stepped over rather than aimed at, so the car never turns back for a
	 * waypoint it has already passed. Falls through to the goal if the whole
	 * remaining plan is behind - then a turn really is the only way on. */
	arc = 0;
	bestX = gJerAi.path.wx[n - 1];
	bestZ = gJerAi.path.wz[n - 1];

	for (k = 1; k < n; k++)
	{
		long ax = gJerAi.path.wx[k - 1], az = gJerAi.path.wz[k - 1];
		long bx = gJerAi.path.wx[k], bz = gJerAi.path.wz[k];
		long dx = bx - ax, dz = bz - az;
		long segLen = JerAiSegLen(ax, az, bx, bz);
		long steps, s;
		int done = 0;

		if (segLen <= 0)
		{
			arc += segLen;
			continue;
		}

		steps = segLen / 512 + 1;

		for (s = 0; s <= steps; s++)
		{
			long f = segLen * s / steps;
			long px = ax + dx * f / segLen;
			long pz = az + dz * f / segLen;
			long thisArc = arc + f;

			if (thisArc < bestArc)
				continue;	/* still behind our own projection */

			if (thisArc - bestArc >= gJerAi.lookAhead)
			{
				long rx = px - cx, rz = pz - cz;
				long rlen2 = rx * rx + rz * rz;
				long cosScaled;

				if (rlen2 <= 0)
				{
					bestX = px;
					bestZ = pz;
					done = 1;
				}
				else
				{
					cosScaled = ((long)fwdX * rx + (long)fwdZ * rz) / (long)(sqrt((double)rlen2) + 1.0);

					if (cosScaled > JERAI_AIM_BEHIND_COS)
					{
						bestX = px;
						bestZ = pz;
						done = 1;
					}
				}

				if (done)
					break;
			}
		}

		if (done)
			break;

		arc += segLen;
	}

	if (aimX) *aimX = (int)bestX;
	if (aimZ) *aimZ = (int)bestZ;
	gJerAi.aimX = (int)bestX;
	gJerAi.aimZ = (int)bestZ;

	return 1;
}

/* ------------------------------------------------------------------ */
/* the pad                                                              */
/* ------------------------------------------------------------------ */

/* is a car roughly ahead of us, close enough to matter? returns its heading
 * offset (signed) when it is, or a sentinel when it is not. Only the cars that
 * are truly close, and roughly in our lane, count: a wider net brakes for
 * oncoming traffic and makes the car crawl. */
#define JERAI_AHEAD_NONE	0x7fffffff

static int JerAiCarAhead(CAR_DATA* car)
{
	int i;
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	int range = gJerAi.avoidDist;
	long range2;

	if (range > 1200)
		range = 1200;	/* a car does not brake for traffic half a street away */

	range2 = (long)range * range;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* o = &car_data[i];
		int ox, oz, rel;
		long d2;

		if (o == car || o->controlType == CONTROL_TYPE_NONE)
			continue;

		ox = o->hd.where.t[0] - cx;
		oz = o->hd.where.t[2] - cz;
		d2 = (long)ox * ox + (long)oz * oz;

		if (d2 > range2)
			continue;

		rel = ((ratan2(ox, oz) - car->hd.direction + 2048) & 0xfff) - 2048;

		if (rel > -450 && rel < 450)
			return rel;	/* within ~40 deg of our nose */
	}

	return JERAI_AHEAD_NONE;
}

/* ------------------------------------------------------------------ */
/* feelers                                                             */
/* ------------------------------------------------------------------ */
/* Short, fresh, thin CellEmpty probes fanned in front of the car. The A* grid is
 * coarse (512-unit samples), the window is only rebuilt every replan_ms, and
 * CellEmpty deliberately ignores smashables - so there are places the plan does
 * not have the full picture. These feelers look at the REAL scenery just ahead
 * and produce a steering bias (and a nose-blocked flag), which nudges the pursuit
 * aim around what the plan missed. They are short on purpose: a long probe reacts
 * to walls the car would have turned before reaching.
 *
 * A wall on the LEFT pushes the aim RIGHT (a negative angle) and the reverse,
 * so a street with walls both sides cancels out and only a genuine drift toward
 * one is corrected. */

static int JerAiFeelClear(CAR_DATA* car, int off, int dist, int radius)
{
	int dir = (car->hd.direction + off) & 0xfff;
	VECTOR v;

	v.vx = car->hd.where.t[0] + (int)((long)dist * RSIN(dir) / 4096);
	v.vz = car->hd.where.t[2] + (int)((long)dist * RCOS(dir) / 4096);
	v.vy = MapHeight(&v);	/* probe at the ground THERE, not at our own height */

	return CellEmpty(&v, radius) != 0;
}

static int JerAiFeelers(CAR_DATA* car, int* frontBlocked)
{
	int bias = 0;

	*frontBlocked = 0;

	/* left feelers push right (negative), right feelers push left (positive) */
	if (!JerAiFeelClear(car, 512, 900, 200))	bias -= 300;
	if (!JerAiFeelClear(car, 256, 1200, 200))	bias -= 150;
	if (!JerAiFeelClear(car, -256, 1200, 200))	bias += 150;
	if (!JerAiFeelClear(car, -512, 900, 200))	bias += 300;

	if (!JerAiFeelClear(car, 0, 800, 240))		*frontBlocked = 1;

	return bias;
}

/* Stuck recovery. A car wedged against scenery (or spun so its aim is behind
 * it) will sit at ~0 speed forever if left to steer forward; after a moment of
 * that, back up while turning, which frees it and lets the next plan take. */
#define JERAI_STUCK_FRAMES	40	/* ~1.3 s at the fixed 30 Hz logic rate */
#define JERAI_RECOVER_FRAMES	25

static int sStuckFrames;
static int sRecoverFrames;
static int sHasMoved;	/* only start counting "stuck" once the car has driven */

static unsigned int JerAiSteerPadImpl(CAR_DATA* car, int aimX, int aimZ)
{
	int dx = aimX - car->hd.where.t[0];
	int dz = aimZ - car->hd.where.t[2];
	int want = ratan2(dx, dz) & 0xfff;
	int frontBlocked = 0;
	int bias = JerAiFeelers(car, &frontBlocked);
	int diff, adiff, spd;
	int ahead;
	unsigned int pad = CAR_PAD_ACCEL;

	gJerAi.lastBias = bias;

	/* nudge the aim around what the plan could not see (the feelers) */
	want = (want + bias) & 0xfff;

	diff = ((want - car->hd.direction + 2048) & 0xfff) - 2048;
	adiff = diff < 0 ? -diff : diff;
	spd = car->hd.speed;

	gJerAi.lastDiff = diff;

	if (spd < 0)
		spd = -spd;

	/* --- stuck detection and the reverse burst it triggers --- */
	if (spd >= 15)
	{
		sHasMoved = 1;
		sStuckFrames = 0;
	}
	else if (sHasMoved)
	{
		sStuckFrames++;
	}

	if (sRecoverFrames > 0)
	{
		sRecoverFrames--;

		/* back up, turning toward the aim side, to peel off the wall */
		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT);
	}

	if (sStuckFrames >= JERAI_STUCK_FRAMES)
	{
		sStuckFrames = 0;
		sRecoverFrames = JERAI_RECOVER_FRAMES;

		/* the plan is what led us here: throw it away so the next frame re-plans
		 * from wherever the reverse burst leaves us, and ask the behavior for a
		 * destination it can actually reach */
		gJerAi.havePlan = 0;
		gJerAi.forceNewGoal = 1;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT);
	}

	/* steer: a small dead-band so the car does not saw at the wheel */
	if (adiff > 60)
		pad |= (diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT;

	/* NO braking for a heading error. The engine already adds autoBrake while
	 * the wheel is turned (handling.c), and stopping the car is exactly what
	 * stops it turning - a stationary car pivots at full lock and never comes
	 * round. Keep the throttle on and let the steer do the work. */

	/* A car close ahead in our lane: lift off the throttle. NO brake - braking
	 * for traffic is what made the car crawl down a street with oncoming cars.
	 * The engine handles any contact; this only stops us driving into it. */
	ahead = JerAiCarAhead(car);

	if (ahead != JERAI_AHEAD_NONE)
		pad &= ~(unsigned int)CAR_PAD_ACCEL;

	/* The nose is genuinely blocked - a FRESH probe, not the coarse grid: brake.
	 * The feeler bias above steers around it; this is the backstop for when there
	 * is nothing to steer around. */
	if (frontBlocked)
	{
		pad &= ~(unsigned int)CAR_PAD_ACCEL;
		pad |= CAR_PAD_BRAKE;
	}

	/* Walls are the A* path's job: it routes around blocked samples, and it
	 * re-plans every replan_ms. A forward wall probe here only fought the plan -
	 * braking for corners the path was about to take, and (with the old
	 * turn-to-the-roomier-side) steering the car into the wall it was braking
	 * for. If the plan is ever wrong and the car wedges, the stuck recovery
	 * above backs it out. */

	return pad;
}

/* The public entry: wraps the implementation so the last pad is available for
 * the once-a-second log without threading it out by hand. */
unsigned int JerAiDriveSteerPad(CAR_DATA* car, int aimX, int aimZ)
{
	unsigned int pad = JerAiSteerPadImpl(car, aimX, aimZ);

	gJerAi.lastPad = (int)pad;

	return pad;
}
