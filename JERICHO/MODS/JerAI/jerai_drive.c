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
#include "civ_ai.h"		/* speedLimits, junctionLightsPhase: the road limit and the lights */
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

int JerAiDriveBuildMap(CAR_DATA* car)
{
	if (car == NULL)
		return 0;

	/* One build per frame: a behavior that re-picks its goal builds the window
	 * to choose on, and the forced re-plan right after it used to build the
	 * same window again - 2 x 2,401 engine probes in one frame. */
	if (gJerAi.mapBuiltAt == gJerAi.tick && JerAiMapValid(&gJerAi.map))
		return 1;

	if (!JerAiMapBuild(&gJerAi.map, car->hd.where.t[0], car->hd.where.t[1], car->hd.where.t[2]))
		return 0;

	gJerAi.mapBuiltAt = gJerAi.tick;

	return 1;
}

int JerAiDrivePlanTo(CAR_DATA* car, int toX, int toZ, int force)
{
	int cx, cz, age, stale;

	if (car == NULL)
		return 0;

	cx = car->hd.where.t[0];
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
	if (!JerAiDriveBuildMap(car))
		return gJerAi.havePlan;

	gJerAi.map.turnCost = gJerAi.turnCost;

	/* While being CHASED, the pull back to the road is what makes the car lose
	 * its line and its speed, so it is weakened (see JERAI_CFG_PANIC_OFFROAD). */
	gJerAi.map.offroadCost = gJerAi.panicFlee ? gJerAi.panicOffroad : gJerAi.offroadCost;
	gJerAi.incomplete = !JerAiStarPlan(&gJerAi.map, cx, cz, toX, toZ, &gJerAi.path);
	gJerAi.plannedAt = gJerAi.tick;
	gJerAi.goalX = toX;
	gJerAi.goalZ = toZ;
	gJerAi.havePlan = (gJerAi.path.waypoints > 0);

	return gJerAi.havePlan;
}

static long long JerAiSegLen(long long ax, long long az, long long bx, long long bz)
{
	double dx = (double)(bx - ax);
	double dz = (double)(bz - az);

	return (long long)sqrt(dx * dx + dz * dz);
}

/* Where the car sits ALONG the plan: the arc length to the closest point of the
 * polyline. 0 when there is no plan. The math is the pure, 64-bit
 * JerAiPathProjectArc (jerai_mapgrid.c): with a 32-bit `long` (Windows) the old
 * inline copy wrapped on any segment past ~1,300 units. */
static long long JerAiProjectArc(int cx, int cz, long long* crossD2)
{
	return JerAiPathProjectArc(gJerAi.path.wx, gJerAi.path.wz, gJerAi.path.waypoints, cx, cz, crossD2);
}

JERAI_CURVE JerAiScanCurve(int cx, int cz, int horizon)
{
	JERAI_CURVE c;
	int n = gJerAi.path.waypoints, k;
	long long startArc, arc = 0;
	int sharp = 0, sharpDist = 0, sharpSign = 0, sharpSpan = 0, haveCurrent = 0;

	c.sharp = 0;
	c.dist = 0;
	c.sign = 0;
	c.current = 0;
	c.span = 0;

	if (!gJerAi.havePlan || n < 3)
		return c;

	startArc = JerAiProjectArc(cx, cz, NULL);

	/* At each interior waypoint, the turn is the heading change from the leg in
	 * to the leg out. Track the biggest within the horizon (so the speed model
	 * slows for the real corner, not a kink), and the very next one ahead. */
	for (k = 1; k < n - 1; k++)
	{
		int ax = gJerAi.path.wx[k - 1], az = gJerAi.path.wz[k - 1];
		int bx = gJerAi.path.wx[k], bz = gJerAi.path.wz[k];
		int ex = gJerAi.path.wx[k + 1], ez = gJerAi.path.wz[k + 1];
		long long legIn = JerAiSegLen(ax, az, bx, bz);
		long long legOut = JerAiSegLen(bx, bz, ex, ez);
		int inDir = ratan2(bx - ax, bz - az) & 0xfff;
		int outDir = ratan2(ex - bx, ez - bz) & 0xfff;
		int turn = ((outDir - inDir + 2048) & 0xfff) - 2048;
		long long distAhead;

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
static int JerAiSpeedTargetRaw(void);

/* The corner/brake model's speed, under the behavior's own cruise cap when it
 * has one (Evade's pace over the road limit; 0 = no cap). */
int JerAiSpeedTarget(void)
{
	int v = JerAiSpeedTargetRaw();

	if (gJerAi.speedCap > 0 && v > gJerAi.speedCap)
		v = gJerAi.speedCap;

	return v;
}

static int JerAiSpeedTargetRaw(void)
{
	const JERAI_CURVE* c = &gJerAi.curve;
	int r, vCorner, vNow;
	double turnRad;

	/* PANIC: running from the police, the point is to be gone, not to be tidy.
	 * Ignore the corner model and the aggression dial entirely and ask for the
	 * car's whole top speed - the steering, the feelers and the stuck recovery
	 * still keep it off the walls. */
	if (gJerAi.panicFlee)
		return gJerAi.topSpeed;

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
	int n, cx, cz, dir, fwdX, fwdZ;
	int bestX = 0, bestZ = 0, segDx = 0, segDz = 0;
	long long bestD2 = -1, bestArc;
	long long la;

	if (car == NULL || !gJerAi.havePlan || gJerAi.path.waypoints <= 0)
		return 0;

	/* a longer look-ahead at speed: smoother, no sawing */
	la = (long long)gJerAi.lookAhead + (long long)JerAiAbs(car->hd.speed) * JERAI_AIM_SPEED_GAIN;

	/* ...and a MUCH longer one while being chased: aiming further down the road
	 * is what lets the car commit to a line and keep its speed up, instead of
	 * correcting short and often (see JERAI_CFG_PANIC_LOOKAHEAD). */
	if (gJerAi.panicFlee)
		la += gJerAi.panicLookAhead;

	gJerAi.aimLook = (int)la;   /* the once-a-second log reads this */

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

		gJerAi.aimX = cx + (int)(4000LL * RSIN(d) / 4096);
		gJerAi.aimZ = cz + (int)(4000LL * RCOS(d) / 4096);

		if (aimX) *aimX = gJerAi.aimX;
		if (aimZ) *aimZ = gJerAi.aimZ;

		return 1;
	}

	/* where the car is ALONG the plan: the closest point on the polyline */
	bestArc = JerAiProjectArc(cx, cz, &bestD2);

	/* cross-track: how far the car sits off the planned line - the term that
	 * turns a small lateral error into a large bearing error when the look-ahead
	 * is short. Magnitude only; the steer trace reads it. */
	gJerAi.aimOff = (bestD2 > 0) ? (int)sqrt((double)bestD2 + 0.5) : 0;

	/* Walk FORWARD from there and take the first point that is both at least
	 * look_ahead further along AND not behind the car (the DIRECTIONAL GUIDE).
	 * Skimming the segment in ~512-unit steps means a point behind the car is
	 * stepped over rather than aimed at, so the car never turns back for a
	 * waypoint it has already passed. Falls through to the goal if the whole
	 * remaining plan is behind - then a turn really is the only way on. The
	 * walk is the pure JerAiPathAimPoint (jerai_mapgrid.c, host-tested). */
	JerAiPathAimPoint(gJerAi.path.wx, gJerAi.path.wz, n, cx, cz, fwdX, fwdZ,
		bestArc, la, JERAI_AIM_BEHIND_COS, &bestX, &bestZ, &segDx, &segDz);

	if (aimX) *aimX = bestX;
	if (aimZ) *aimZ = bestZ;
	gJerAi.aimX = bestX;
	gJerAi.aimZ = bestZ;

	/* how far ahead the aim actually landed - the once-a-second log prints it,
	 * and it is the honest readout for "is it aiming down the road or at its own
	 * bumper" (a look-ahead the plan cannot satisfy falls back to the goal). */
	gJerAi.aimDist = (int)sqrt((double)JerAiDist2(bestX, bestZ, cx, cz) + 0.5);

	/* the PATH's own direction where the aim sits - the bearing of the plan
	 * segment that carries it, NOT the bearing from the car to it (that is
	 * `want`, and using it here would make the readout tautological). With
	 * aimOff this splits the heading error into "the car is not aligned with
	 * the path" and "the aim sits off to one side of the car". */
	gJerAi.aimPathDir = (segDx != 0 || segDz != 0) ? (ratan2(segDx, segDz) & 0xfff) : 0;

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

static int JerAiCarAhead(CAR_DATA* car, long long* dist2Out, int* relOut)
{
	int i;
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	int range = gJerAi.avoidDist;
	long long range2;
	long long bestD2 = -1;
	int bestRel = 0;

	if (dist2Out) *dist2Out = -1;
	if (relOut) *relOut = 0;

	if (range > JERAI_AVOID_MAX)
	{
		/* The cap is deliberate - a car does not brake for traffic half a street
		 * away - but it used to be SILENT, so `avoid_dist = 1800` in the config
		 * did nothing at all and looked broken. Say so once. */
		static int warned;

		if (!warned && gJerAi.ctx != NULL)
		{
			gJerAi.ctx->jer_log(gJerAi.ctx,
				"[JerAI] avoid_dist %d exceeds the %d cap; using %d\n",
				range, JERAI_AVOID_MAX, JERAI_AVOID_MAX);
			warned = 1;
		}

		range = JERAI_AVOID_MAX;
	}

	range2 = (long long)range * range;

	for (i = 0; i < MAX_CARS; i++)
	{
		CAR_DATA* o = &car_data[i];
		int ox, oz, rel;
		long long d2;

		if (o == car || o->controlType == CONTROL_TYPE_NONE)
			continue;

		/* A car we are FOLLOWING (or running from) is the destination, not
		 * traffic: swerving around it and braking for it is what made a follower
		 * try to overtake the car it was told to tail. The follow clearance is
		 * what keeps the two apart. */
		if (i == gJerAi.followCarId)
			continue;

		ox = o->hd.where.t[0] - cx;
		oz = o->hd.where.t[2] - cz;
		/* 64-bit: with a 32-bit long a car ~46,000+ units away wrapped NEGATIVE,
		 * passed this gate and counted as right in front (phantom brake/swerve) */
		d2 = (long long)ox * ox + (long long)oz * oz;

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

	v.vx = car->hd.where.t[0] + (int)((long long)dist * RSIN(dir) / 4096);
	v.vz = car->hd.where.t[2] + (int)((long long)dist * RCOS(dir) / 4096);

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

/* RELEASE HYSTERESIS - the obstacle response needs its own, and this is where.
 *
 * A probe that has read blocked keeps counting for a few frames after it first
 * reads clear. Without it the bias has hard edges: pass a pole and the 300 (or
 * 150) that feeler contributed vanishes the SAME frame the probe clears, which
 * steps the aim by up to 26 deg and hands the heading loop a step input - the
 * wheel saws and the car fishtails. Holding the edge turns the step into a ramp.
 *
 * The NOSE is held too, on purpose: its only consumer is a brake, and a brake
 * that outlives the obstacle by 0.2 s is strictly safer than stamping back on
 * the throttle the instant the grille clears. (The wall-evasion and the
 * nose-on-scenery-at-a-standstill probes below are deliberately NOT held - they
 * are the emergency paths, and there a fresh reading is the honest one.) */
#define JERAI_FEEL_HOLD	6	/* frames a cleared feeler keeps counting */

static int sFeelHold[5];	/* [0..3] the four side feelers, [4] the nose */

static int JerAiFeelBlocked(int which, int clear)
{
	if (!clear)
	{
		sFeelHold[which] = JERAI_FEEL_HOLD;
		return 1;
	}

	if (sFeelHold[which] > 0)
	{
		sFeelHold[which]--;
		return 1;
	}

	return 0;
}

static int JerAiFeelers(CAR_DATA* car, int* frontBlocked)
{
	int bias = 0;

	*frontBlocked = 0;

	/* left feelers push right (negative), right feelers push left (positive) */
	if (JerAiFeelBlocked(0, JerAiFeelClear(car, 512, 900, 200)))	bias -= 300;
	if (JerAiFeelBlocked(1, JerAiFeelClear(car, 256, 1200, 200)))	bias -= 150;
	if (JerAiFeelBlocked(2, JerAiFeelClear(car, -256, 1200, 200)))	bias += 150;
	if (JerAiFeelBlocked(3, JerAiFeelClear(car, -512, 900, 200)))	bias += 300;

	if (JerAiFeelBlocked(4, JerAiFeelClear(car, 0, 800, 240)))	*frontBlocked = 1;

	return bias;
}

/* SLEW LIMIT on the aim nudge. The hold above ramps a single feeler, but several
 * can still flip in one frame (a gap between two probes opens and closes) and the
 * traffic bias then adds another 220 on top, so the TOTAL is moved toward its new
 * value by at most JERAI_BIAS_SLEW a frame rather than assigned. A step into the
 * aim is a step into the heading loop, and that is what the wheel saws on. */
#define JERAI_BIAS_SLEW	120	/* aim units per frame */

static int sBiasApplied;

static int JerAiBiasSlew(int want)
{
	int d = want - sBiasApplied;

	if (d > JERAI_BIAS_SLEW)
		d = JERAI_BIAS_SLEW;
	else if (d < -JERAI_BIAS_SLEW)
		d = -JERAI_BIAS_SLEW;

	sBiasApplied += d;

	return sBiasApplied;
}

/* Stuck recovery. A car wedged against scenery (or spun so its aim is behind
 * it) will sit at ~0 speed forever if left to steer forward; after a moment of
 * that, back up while turning, which frees it and lets the next plan take.
 *
 * THE STEER BIT IS INVERTED IN A BURST, and that is not a typo. CAR_PAD_BRAKE
 * at a standstill is REVERSE thrust (handling.c: `-rws < 23` -> a fixed -5000),
 * and steering while reversing rotates the nose the OPPOSITE way to the same
 * bit going forward. Steering "toward the aim side" with the forward sign while
 * backing up therefore swings the nose AWAY from where the car wants to go, so
 * every burst left the heading error worse than it found it and the car
 * re-wedged - the observed 15-second reverse loop. */
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
	long long aheadD2 = -1;
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

	}

	/* nudge the aim around what the plan could not see (the feelers + traffic).
	 * SLEWED, not assigned - see JerAiBiasSlew. */
	bias = JerAiBiasSlew(bias);
	gJerAi.lastBias = bias;

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

	gJerAi.stuckFrames = sStuckFrames;
	gJerAi.recoverFrames = sRecoverFrames;

	if (sRecoverFrames > 0)
	{
		sRecoverFrames--;

		/* back up, steering so the NOSE swings toward the aim (the bit is
		 * inverted because we are reversing - see JERAI_RECOVER_FRAMES) */
		gJerAi.recoverFrames = sRecoverFrames;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	if (sStuckFrames >= JERAI_STUCK_FRAMES)
	{
		sStuckFrames = 0;
		sRecoverFrames = JERAI_RECOVER_FRAMES;

		gJerAi.stuckFrames = 0;
		gJerAi.recoverFrames = sRecoverFrames;

		/* the plan is what led us here: throw it away so the next frame re-plans
		 * from wherever the reverse burst leaves us, and ask the behavior for a
		 * destination it can actually reach - WILD, i.e. anywhere in the window,
		 * so the car relocates instead of niggling at the same wedge */
		gJerAi.havePlan = 0;
		gJerAi.forceNewGoal = 1;
		gJerAi.reposition = 1;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
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

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
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

		gJerAi.lastPred = pred;	/* the steer trace reads this */

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

		if (aheadD2 >= 0 && aheadD2 < 750LL * 750LL)
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

	/* WHEELSPIN launch. CAR_PAD_WHEELSPIN makes the engine apply MAXIMUM thrust
	 * regardless of the pedal (wheelforces.c: thrust = powerRatio * 5000), which
	 * is how a chased car gets off the line. It buys that by loading the front
	 * tyre force and starving the rear (`*rearFS = 0x780 - *frontFS`), so a
	 * wheelspun car has little rear grip and spins out the moment it is asked to
	 * turn - and Circle is the handbrake's opposite rather than a free button.
	 *
	 * So it is used ONLY to get going in a straight line while running for it,
	 * and the pad goes back to plain throttle the moment the car has to point
	 * anywhere. The guard is the FRONT WHEEL ANGLE and the heading error, not
	 * the pad bits: the bits lead the wheel by a frame or two, so requiring "no
	 * steering bit" as well asked for something that almost never holds and made
	 * the whole thing a no-op. The engine also cancels wheelspin by itself above
	 * a wheel speed, which is the last line of defence. */
	if (gJerAi.panicFlee && gJerAi.panicWheelspin && car != NULL &&
	    (pad & CAR_PAD_ACCEL) && JerAiAbs(gJerAi.lastDiff) < 150 &&
	    JerAiAbs(car->wheel_angle) < 80)
	{
		pad = (pad & ~(unsigned int)CAR_PAD_ACCEL) | (unsigned int)CAR_PAD_WHEELSPIN;
		gJerAi.lastPad = (int)pad;
	}

	/* STEERING CAP (Evade's `max_steer`): stop adding lock once the front wheel
	 * is at the cap. JerAI steers with the regular D-pad rate, which the engine
	 * tops out at 352 (handling.c), so a cap of 352 or more changes nothing. Not
	 * applied to the emergency paths (the wall-evasion handbrake turn, the
	 * reverse burst), which need the whole lock. */
	if (car != NULL && gJerAi.steerCap > 0 && gJerAi.steerCap < 352 &&
	    !gJerAi.wantHandbrake && gJerAi.recoverFrames == 0)
	{
		if ((pad & CAR_PAD_RIGHT) && car->wheel_angle >= gJerAi.steerCap)
			pad &= ~(unsigned int)CAR_PAD_RIGHT;

		if ((pad & CAR_PAD_LEFT) && car->wheel_angle <= -gJerAi.steerCap)
			pad &= ~(unsigned int)CAR_PAD_LEFT;

		gJerAi.lastPad = (int)pad;
	}

	/* the ACTUAL front wheel angle, read back after the engine applied the pad
	 * (30 Hz logic, one frame behind). This is the readout that shows the wheel
	 * being centred between corrections - see JERAI_CFG_STEER_TRACE. */
	if (car != NULL)
		gJerAi.lastWheel = car->wheel_angle;

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

	/* parked on purpose is not wedged: the stop itself must not count toward the
	 * stuck timer, or pulling away again (a light going green, the followed car
	 * leaving) starts most of the way to a reverse burst */
	sStuckFrames = 0;
	gJerAi.stuckFrames = 0;

	pad = (spd > 8) ? CAR_PAD_BRAKE : 0;

	gJerAi.lastPad = (int)pad;

	return pad;
}

/* A new level (JER_EVENT_GAME_START): everything here describes the LAST one. */
void JerAiDriveResetRun(void)
{
	int i;

	for (i = 0; i < 5; i++)
		sFeelHold[i] = 0;

	sBiasApplied = 0;
	sStuckFrames = 0;
	sRecoverFrames = 0;
	sHasMoved = 0;

	gJerAi.havePlan = 0;
	gJerAi.incomplete = 0;
	gJerAi.path.waypoints = 0;
	gJerAi.path.complete = 0;
	gJerAi.plannedAt = 0;
	gJerAi.goalX = gJerAi.goalZ = 0;
	gJerAi.aimX = gJerAi.aimZ = 0;
	gJerAi.map.valid = 0;
	gJerAi.mapBuiltAt = -1;
	gJerAi.prevDir = -1;		/* no last heading: the first frame has no yaw */
	gJerAi.lastSteerDir = 0;
	gJerAi.lastDiff = gJerAi.lastBias = gJerAi.lastPred = 0;
	gJerAi.wantHandbrake = 0;
	gJerAi.stuckFrames = gJerAi.recoverFrames = 0;
	gJerAi.forceNewGoal = 0;
	gJerAi.reposition = 0;
	gJerAi.tanActive = 0;
	gJerAi.followCarId = -1;
	gJerAi.panicFlee = 0;
	gJerAi.speedCap = 0;
	gJerAi.steerCap = 0;
}

/* ------------------------------------------------------------------ */
/* the road: its speed limit and its traffic lights (Evade)             */
/* ------------------------------------------------------------------ */

/* The road surface under a world point, probed at the ground there (the same
 * way the occupancy grid reads JerRoadAt). 0 off the level or off the road. */
static int JerAiRoadUnder(int x, int z, JER_ROAD_INFO* out)
{
	VECTOR v;

	if (x + units_across_halved < 0 || x - units_across_halved >= 0 ||
	    z + units_down_halved < 0 || z - units_down_halved >= 0)
		return 0;

	v.vx = x;
	v.vy = 0;
	v.vz = z;
	v.vy = MapHeight(&v);

	return JerRoadInfoAt(x, v.vy, z, out);
}

int JerAiDriveRoadLimit(CAR_DATA* car, int fallback)
{
	JER_ROAD_INFO info;
	int id;

	if (car == NULL || !JerAiRoadUnder(car->hd.where.t[0], car->hd.where.t[2], &info))
		return fallback;

	/* a junction carries no lane data (dr2roads.c JerRoadInfoAt): keep the limit
	 * of the road the car came from */
	if (info.kind == 2)
		return fallback;

	/* the civ AI's own table, indexed the way civ_ai.c / felony.c index it */
	id = info.speedLimit;

	if (id < 0) id = 0;
	if (id > 2) id = 2;

	return speedLimits[id];
}

/* The junction-ahead light test, read the way the engine's own red-light felony
 * reads it (felony.c CheckPlayerMiscFelonies): the junction has lights when
 * `flags & 1`, the exit the car enters by is the one whose ExitIdx is the road it
 * is on, and that exit's phase is junctionLightsPhase[exit & 1] - 3 green, 2
 * amber, 1 red (civ_ai.c TrafficLightCycle). Probes straight down the car's
 * heading in 300-unit steps out to `range`. */
int JerAiDriveStopLightAhead(CAR_DATA* car, int range, int* distOut)
{
	JER_ROAD_INFO here, ahead;
	int cx, cz, dir, d, i;

	if (distOut != NULL)
		*distOut = 0;

	if (car == NULL)
		return 0;

	cx = car->hd.where.t[0];
	cz = car->hd.where.t[2];
	dir = car->hd.direction & 0xfff;

	/* already in the junction (or off the road): never stop half way across */
	if (!JerAiRoadUnder(cx, cz, &here) || here.kind == 2)
		return 0;

	for (d = 300; d <= range; d += 300)
	{
		int px = cx + (int)((long long)d * RSIN(dir) / 4096);
		int pz = cz + (int)((long long)d * RCOS(dir) / 4096);
		int exitId = -1, phase;

		if (!JerAiRoadUnder(px, pz, &ahead))
			continue;

		if (ahead.kind != 2)
			continue;	/* still road: keep looking */

		/* the first junction ahead decides: no lights, or not entered from
		 * this road (a turn across it), is not ours to wait for */
		if (ahead.surfId < 0 || !(GET_JUNCTION(ahead.surfId)->flags & 1))
			return 0;

		for (i = 0; i < 4; i++)
		{
			if (ahead.connect[i] == here.surfId)
			{
				exitId = i;
				break;
			}
		}

		if (exitId < 0)
			return 0;

		phase = junctionLightsPhase[exitId & 1];

		if (phase == 3)
			return 0;	/* green */

		/* amber close to the line: go, the way a driver does */
		if (phase == 2 && d <= 600)
			return 0;

		if (distOut != NULL)
			*distOut = d;

		return 1;
	}

	return 0;
}
