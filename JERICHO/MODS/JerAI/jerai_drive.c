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

int JerAiDriveAim(int* aimX, int* aimZ)
{
	CAR_DATA* car = JerAiOwnCar();
	int cx, cz, k, nearest = 0, best;
	long look2, bestd = -1;

	if (car == NULL || !gJerAi.havePlan || gJerAi.path.waypoints <= 0)
		return 0;

	cx = car->hd.where.t[0];
	cz = car->hd.where.t[2];
	look2 = (long)gJerAi.lookAhead * gJerAi.lookAhead;

	/* the nearest waypoint to the car: the path portion behind it is not
	 * somewhere to aim at. Aiming at "the furthest within the look-ahead"
	 * without this let the car target a waypoint it had already PASSED, so the
	 * error pinned to +/-180 deg and the car spun on the spot. */
	for (k = 0; k < gJerAi.path.waypoints; k++)
	{
		int dx = gJerAi.path.wx[k] - cx;
		int dz = gJerAi.path.wz[k] - cz;
		long d2 = (long)dx * dx + (long)dz * dz;

		if (bestd < 0 || d2 < bestd)
		{
			bestd = d2;
			nearest = k;
		}
	}

	/* then the furthest waypoint FORWARD of that one within the look-ahead, so
	 * the car drives through corners instead of clipping the next grid step */
	best = nearest;

	for (k = nearest + 1; k < gJerAi.path.waypoints; k++)
	{
		int dx = gJerAi.path.wx[k] - cx;
		int dz = gJerAi.path.wz[k] - cz;

		if ((long)dx * dx + (long)dz * dz <= look2)
			best = k;
		else
			break;
	}

	if (best == nearest && nearest + 1 < gJerAi.path.waypoints)
		best = nearest + 1;

	if (aimX) *aimX = gJerAi.path.wx[best];
	if (aimZ) *aimZ = gJerAi.path.wz[best];
	gJerAi.aimX = gJerAi.path.wx[best];
	gJerAi.aimZ = gJerAi.path.wz[best];

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
	int diff = ((want - car->hd.direction + 2048) & 0xfff) - 2048;
	int adiff = diff < 0 ? -diff : diff;
	int spd = car->hd.speed;
	int ahead;
	unsigned int pad = CAR_PAD_ACCEL;

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
		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
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

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	/* steer: a small dead-band so the car does not saw at the wheel */
	if (adiff > 60)
		pad |= (diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT;

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
