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
#include "map.h"		/* units_across_halved: the level's bounds */
#include "cell.h"		/* Get*PackedCop, CELL_ITERATOR */
#include "models.h"		/* modelpointers */
#include "draw.h"		/* matrixtable */
/* GET_MODEL_DATA / COLLISION_PACKET / MODEL_FLAG_* arrive via driver2.h ->
 * dr2types.h -> engine/mdl.h */

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

JERAI_CARSTATE JerAiReadCar(CAR_DATA* car)
{
	JERAI_CARSTATE s;
	int vx, vz, vdir;

	s.speed = car->hd.wheel_speed;
	s.dir = car->hd.direction;
	s.yawRate = car->st.n.angularVelocity[1];
	s.yaw = 0;
	s.slip = 0;

	/* the ACTUAL heading change since last frame, in the same game units as the
	 * heading error - the raw angular velocity is in engine fixed-point and the
	 * two are not directly comparable, so derive the yaw this way instead. This
	 * is what the predictive steering leads by. */
	if (gJerAi.prevDir >= 0)
	{
		s.yaw = ((s.dir - gJerAi.prevDir + 2048) & 0xfff) - 2048;

		if (s.yaw > 256) s.yaw = 256;
		if (s.yaw < -256) s.yaw = -256;
	}

	gJerAi.prevDir = s.dir;

	/* st.n.fposition is where<<4, so linearVelocity lives in that same <<4
	 * frame; the ANGLE between it and the heading is scale-independent, which
	 * is all we read it for. A slip near 0 means the car is hooked up; a large
	 * slip means the body is not pointing where it is going - a slide. */
	vx = car->st.n.linearVelocity[0];
	vz = car->st.n.linearVelocity[2];

	if (vx != 0 || vz != 0)
	{
		vdir = ratan2(vx, vz) & 0xfff;
		s.slip = ((vdir - s.dir + 2048) & 0xfff) - 2048;
	}

	return s;
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

	gJerAi.map.turnCost = gJerAi.turnCost;
	gJerAi.map.offroadCost = gJerAi.offroadCost;
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

/* Where the car sits ALONG the plan: the arc length to the closest point of the
 * polyline. 0 when there is no plan. */
static long JerAiProjectArc(int cx, int cz)
{
	int n = gJerAi.path.waypoints, k;
	long bestD2 = -1, bestArc = 0, arc = 0;

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

	return bestArc;
}

JERAI_CURVE JerAiScanCurve(int cx, int cz, int horizon)
{
	JERAI_CURVE c;
	int n = gJerAi.path.waypoints, k;
	long startArc, arc = 0;
	int sharp = 0, sharpDist = 0, sharpSign = 0, sharpSpan = 0, haveCurrent = 0;

	c.sharp = 0;
	c.dist = 0;
	c.sign = 0;
	c.current = 0;
	c.span = 0;

	if (!gJerAi.havePlan || n < 3)
		return c;

	startArc = JerAiProjectArc(cx, cz);

	/* At each interior waypoint, the turn is the heading change from the leg in
	 * to the leg out. Track the biggest within the horizon (so the speed model
	 * slows for the real corner, not a kink), and the very next one ahead. */
	for (k = 1; k < n - 1; k++)
	{
		long ax = gJerAi.path.wx[k - 1], az = gJerAi.path.wz[k - 1];
		long bx = gJerAi.path.wx[k], bz = gJerAi.path.wz[k];
		long ex = gJerAi.path.wx[k + 1], ez = gJerAi.path.wz[k + 1];
		long legIn = JerAiSegLen(ax, az, bx, bz);
		long legOut = JerAiSegLen(bx, bz, ex, ez);
		int inDir = ratan2(bx - ax, bz - az) & 0xfff;
		int outDir = ratan2(ex - bx, ez - bz) & 0xfff;
		int turn = ((outDir - inDir + 2048) & 0xfff) - 2048;
		long distAhead;

		arc += legIn;
		distAhead = arc - startArc;

		if (distAhead < 0)
			distAhead = 0;

		if (!haveCurrent && arc >= startArc)
		{
			c.current = turn;
			haveCurrent = 1;
		}

		if (distAhead <= horizon && (turn < 0 ? -turn : turn) > (sharp < 0 ? -sharp : sharp))
		{
			sharp = turn;
			sharpDist = (int)distAhead;
			sharpSign = turn > 0 ? 1 : (turn < 0 ? -1 : 0);
			sharpSpan = (int)(legIn + legOut);
		}
	}

	c.sharp = sharp;
	c.dist = sharpDist;
	c.sign = sharpSign;
	c.span = sharpSpan;

	return c;
}

/* ------------------------------------------------------------------ */
/* the corner-speed model                                               */
/* ------------------------------------------------------------------ */

/* The slip angle at which the car counts as fully sliding: the grip readout
 * falls from 100 (hooked up) to 0 as |slip| approaches this. */
#define JERAI_SLIP_LIMIT	600

/* How far over the corner speed the car must be before the driver brakes rather
 * than merely lifting off (engine speed units). A small margin keeps it from
 * dithering between throttle and brake. */
#define JERAI_BRAKE_BAND	25

/* The speed that keeps the car inside the lateral-g budget through a corner of
 * radius r. corner_g folds the grip and the unit conversions into one knob:
 * bigger = the car carries more speed through the same curve. */
static int JerAiSpeedForRadius(int r)
{
	int v;

	if (r <= 0)
		return gJerAi.topSpeed;

	v = (int)sqrt((double)gJerAi.cornerG * (double)r);

	if (v > gJerAi.topSpeed) v = gJerAi.topSpeed;
	if (v < 8) v = 8;

	return v;
}

/* The speed the driver may carry RIGHT NOW so it can still be down to the corner
 * speed by the time it reaches the corner: v^2 = vCorner^2 + 2*aBrake*dist.
 * This is the momentum term - slowing only once the corner arrives is exactly
 * how a car ploughs into the outside wall. */
int JerAiSpeedTarget(void)
{
	const JERAI_CURVE* c = &gJerAi.curve;
	int r, vCorner, vNow;
	double turnRad;

	if (!gJerAi.havePlan || c->sharp == 0 || c->span <= 0)
		return gJerAi.topSpeed;

	turnRad = (double)(c->sharp < 0 ? -c->sharp : c->sharp) * 6.283185307 / 4096.0;

	if (turnRad < 0.02)
		return gJerAi.topSpeed;

	r = (int)((double)c->span / turnRad);
	vCorner = JerAiSpeedForRadius(r);

	vNow = (int)sqrt((double)vCorner * (double)vCorner + 2.0 * (double)gJerAi.brakeG * (double)c->dist);

	/* the aggression dial: scale the whole thing, then cap at the car's limit */
	if (gJerAi.speedScale > 0 && gJerAi.speedScale != 100)
		vNow = vNow * gJerAi.speedScale / 100;

	if (vNow > gJerAi.topSpeed) vNow = gJerAi.topSpeed;

	return vNow;
}

/* How much grip is left: 100 when hooked up, falling as the car slides. The slip
 * angle is the honest "am I asking for more than the tyres have" measure. */
static int JerAiGripPct(void)
{
	int s = gJerAi.carState.slip;

	if (s < 0)
		s = -s;

	if (s >= JERAI_SLIP_LIMIT)
		return 0;

	return 100 - s * 100 / JERAI_SLIP_LIMIT;
}

/* cos(angle) fixed-point: a point is "not behind" the car when the cosine
 * between the car's heading and the direction to the point exceeds this
 * (-0.35 ~= 110 deg, so a perpendicular intersection turn still counts). */
#define JERAI_AIM_BEHIND_COS	(-1450)

/* How much further up the plan the aim looks per unit of speed. A fixed
 * look-ahead makes a fast car chase every wiggle of the path and saw at the
 * wheel; stretching it with speed is the cheapest cure for that oscillation. */
#define JERAI_AIM_SPEED_GAIN	8

/* The steering lead is configurable (`yaw_lead`, default 4): the number of frames
 * of the car's own yaw the aim leads the heading error by. It damps the wheel so
 * the car does not saw at it or oscillate up a straight, but too much of it
 * straightens the wheel as soon as the car begins turning - which reads as "it
 * steers far too slowly". See JERAI_CFG_YAW_LEAD. */

int JerAiDriveAim(int* aimX, int* aimZ)
{
	CAR_DATA* car = JerAiOwnCar();
	int n, k;
	long cx, cz;
	long bestD2 = -1, bestArc = 0, arc;
	int dir, fwdX, fwdZ;
	long bestX, bestZ;
	long la;

	if (car == NULL || !gJerAi.havePlan || gJerAi.path.waypoints <= 0)
		return 0;

	/* a longer look-ahead at speed: smoother, no sawing */
	la = gJerAi.lookAhead + (long)JerAiAbs(car->hd.speed) * JERAI_AIM_SPEED_GAIN;

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

			if (thisArc - bestArc >= la)
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
 * oncoming traffic and makes the car crawl. Also reports the CLOSEST one's
 * squared distance and which side it sits on, for steering around it. */
#define JERAI_AHEAD_NONE	0x7fffffff

static int JerAiCarAhead(CAR_DATA* car, long* dist2Out, int* relOut)
{
	int i;
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	int range = gJerAi.avoidDist;
	long range2;
	long bestD2 = -1;
	int bestRel = 0;

	if (dist2Out) *dist2Out = -1;
	if (relOut) *relOut = 0;

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

		if (rel <= -450 || rel >= 450)
			continue;	/* not within ~40 deg of our nose */

		/* keep the CLOSEST one, not the first found */
		if (bestD2 < 0 || d2 < bestD2)
		{
			bestD2 = d2;
			bestRel = rel;
		}
	}

	if (bestD2 < 0)
		return JERAI_AHEAD_NONE;

	if (dist2Out) *dist2Out = bestD2;
	if (relOut) *relOut = bestRel;

	return bestRel;
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

	/* OUTSIDE the loaded level is never "clear": CellEmpty reads an out-of-level
	 * cell as EMPTY, so a feeler beyond the map edge would otherwise wave the car
	 * straight off it. The ground is only read once we know it is in-bounds. */
	if (v.vx + units_across_halved < 0 || v.vx - units_across_halved >= 0 ||
	    v.vz + units_down_halved < 0 || v.vz - units_down_halved >= 0)
		return 0;

	v.vy = MapHeight(&v);	/* probe at the ground THERE, not at our own height */

	if (!CellEmpty(&v, radius))
		return 0;

	/* ...and the SAME probe again, this time also counting the smashable props
	 * CellEmpty skips (light posts, fencing). SHARED with the occupancy grid: the
	 * grid marks a sample with those in it as blocked, so the A* routes around
	 * them and the feelers are the short-range backstop for anything the grid is
	 * too coarse to see. */
	if (JerAiMapCellHasSmashable(&v, radius))
		return 0;

	return 1;
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
	int diff, spd;
	int ahead;
	long aheadD2 = -1;
	int aheadRel = 0;
	unsigned int pad = 0;

	/* what the car is doing right now - speed, yaw rate, slide. The controller
	 * reads this; it is the difference between driving and walking a line. */
	gJerAi.carState = JerAiReadCar(car);
	gJerAi.wantHandbrake = 0;

	/* and what the plan is about to do - the corner ahead and how far off */
	gJerAi.curve = JerAiScanCurve(car->hd.where.t[0], car->hd.where.t[2], gJerAi.cornerLookahead);

	/* the grip-limited speed the car may carry into that corner, and how much
	 * grip is left right now */
	gJerAi.targetSpeed = JerAiSpeedTarget();
	gJerAi.gripPct = JerAiGripPct();

	gJerAi.lastBias = bias;

	/* Another car close ahead in our lane: steer around it AND be ready to brake.
	 * The steer bias picks the side with room (or the side AWAY from the car), so
	 * it goes around rather than into it; the brake in the pedal section below is
	 * the backstop for when there is no room to go around at all. */
	ahead = JerAiCarAhead(car, &aheadD2, &aheadRel);

	if (ahead != JERAI_AHEAD_NONE)
	{
		int rightClear = JerAiFeelClear(car, 512, 1400, 200);
		int leftClear = JerAiFeelClear(car, -512, 1400, 200);

		if (rightClear && !leftClear)
			bias += 220;		/* room to the right -> aim right */
		else if (leftClear && !rightClear)
			bias -= 220;		/* room to the left -> aim left */
		else
			bias += (aheadRel > 0) ? -220 : 220;	/* else away from the car */

		gJerAi.lastBias = bias;
	}

	/* nudge the aim around what the plan could not see (the feelers + traffic) */
	want = (want + bias) & 0xfff;

	diff = ((want - car->hd.direction + 2048) & 0xfff) - 2048;
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
		 * destination it can actually reach - WILD, i.e. anywhere in the window,
		 * so the car relocates instead of niggling at the same wedge */
		gJerAi.havePlan = 0;
		gJerAi.forceNewGoal = 1;
		gJerAi.reposition = 1;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT);
	}

	/* --- wall evasion: a tight handbrake turn when the nose gets close -----
	 * The handbrake is NOT sent as a pad bit: handling.c cycles
	 * g_PlayerControlMode on that bit's press edge (manual -> traffic -> cop ->
	 * lead), which would hand the car to the engine's AI. The CAR_PAD hook's
	 * handled path sets cp->handbrake directly, so the mode is never touched. */
	if (spd > 12 && !JerAiFeelClear(car, 0, 1100, 240))
	{
		int rightClear = JerAiFeelClear(car, 512, 1100, 200);
		int leftClear = JerAiFeelClear(car, -512, 1100, 200);

		gJerAi.wantHandbrake = 1;
		pad = 0;	/* a handbrake turn drives no throttle */

		/* turn toward whichever side is open; if neither clearly is, toward the
		 * aim */
		if (rightClear && !leftClear)
			pad |= CAR_PAD_RIGHT;
		else if (leftClear && !rightClear)
			pad |= CAR_PAD_LEFT;
		else
			pad |= (diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT;

		gJerAi.lastSteerDir = 0;

		return pad;
	}

	/* Scenery right on the nose AND barely moving: back up NOW rather than wait
	 * out the whole stuck timer - the earlier the car peels off, the less it
	 * grinds along the wall. */
	if (spd < 15 && !JerAiFeelClear(car, 0, 480, 240))
	{
		sRecoverFrames = JERAI_RECOVER_FRAMES;
		gJerAi.lastSteerDir = 0;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT);
	}

	/* --- speed: brake into the corner, power out of it --------------------
	 * target_speed already folds in the corner AND the braking distance to it,
	 * so simply tracking it is what makes the car slow BEFORE the corner rather
	 * than at it. That is the momentum term the old driver had no idea about. */
	if (car->hd.speed < 0)
	{
		pad = CAR_PAD_ACCEL;		/* reversing: power back out */
	}
	else if (spd > gJerAi.targetSpeed + JERAI_BRAKE_BAND)
	{
		pad = CAR_PAD_BRAKE;		/* well over the corner speed: brake */
	}
	else if (spd > gJerAi.targetSpeed)
	{
		pad = 0;			/* a little over: just lift off */
	}
	else if (spd < gJerAi.targetSpeed - 12)
	{
		pad = CAR_PAD_ACCEL;		/* under it: power out */
	}
	else
	{
		pad = 0;			/* on the target: hold steady */
	}

	/* steer with hysteresis, so it does not saw at the wheel: start turning when
	 * the error passes START, keep turning until it falls back under HOLD, and
	 * never flip sides on noise. */
	{
		int startBand = 70;
		int holdBand = 30;
		/* Lead the heading error with where the nose is ABOUT to be. Heavy cars
		 * carry real rotational momentum, so chasing the current error overshoots;
		 * subtracting yaw*lead lets the wheel straighten - and countersteer - before
		 * the error closes, which is what stops the up-the-street oscillation. */
		int pred = diff - gJerAi.carState.yaw * gJerAi.yawLead;
		int apred = pred < 0 ? -pred : pred;
		int dir = pred > 0 ? 1 : -1;

		if (gJerAi.lastSteerDir == dir && apred > holdBand)
			;					/* stay on it */
		else if (apred > startBand)
			gJerAi.lastSteerDir = dir;
		else
			gJerAi.lastSteerDir = 0;
	}

	if (gJerAi.lastSteerDir > 0)
		pad |= CAR_PAD_RIGHT;
	else if (gJerAi.lastSteerDir < 0)
		pad |= CAR_PAD_LEFT;

	/* No brake for a heading error alone: the engine already adds autoBrake while
	 * the wheel is turned (handling.c), and stopping the car is what stops it
	 * turning. The brake above is only ever for genuine over-speed. */

	/* A car close ahead in our lane: lift off the throttle. NO brake - braking
	 * for traffic is what made the car crawl down a street with oncoming cars.
	 * The engine handles any contact; this only stops us driving into it. */
	/* Another car in our lane: lift, and brake if it is genuinely close. Going
	 * around is the steer bias above; this is the "absolutely do not hit it"
	 * backstop for when there is nowhere to go around. */
	if (ahead != JERAI_AHEAD_NONE)
	{
		pad &= ~(unsigned int)CAR_PAD_ACCEL;

		if (aheadD2 >= 0 && aheadD2 < 750L * 750L)
			pad |= CAR_PAD_BRAKE;
	}

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

/* Hold: come to a stop and stay there. See jerai_drive.h for why this is a
 * separate primitive rather than a flag on the steering pad. */
unsigned int JerAiDriveHoldPad(CAR_DATA* car)
{
	int spd;
	unsigned int pad;

	if (car == NULL)
		return 0;

	spd = car->hd.speed;

	if (spd < 0)
		spd = -spd;

	/* The once-a-second log prints carState and the last-* fields, and this path
	 * never reaches JerAiSteerPadImpl to fill them - so refresh them here or the
	 * readout freezes at whatever the last real steering frame left. */
	gJerAi.carState = JerAiReadCar(car);
	gJerAi.targetSpeed = 0;
	gJerAi.lastDiff = 0;
	gJerAi.lastBias = 0;
	gJerAi.lastSteerDir = 0;
	gJerAi.wantHandbrake = 0;

	pad = (spd > 8) ? CAR_PAD_BRAKE : 0;

	gJerAi.lastPad = (int)pad;

	return pad;
}
