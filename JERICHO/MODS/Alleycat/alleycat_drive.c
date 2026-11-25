/*
 * alleycat_drive.c -- the driving primitives: plan, aim, pad. See alleycat_drive.h.
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

#include "alleycat_internal.h"
#include "alleycat_drive.h"

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

void AlleycatRandSeed(unsigned int s)
{
	sRng = s ? s : 0x12345678u;
}

unsigned int AlleycatRand(void)
{
	sRng = sRng * 1664525u + 1013904223u;
	return sRng >> 16;
}

/* ------------------------------------------------------------------ */
/* the car                                                              */
/* ------------------------------------------------------------------ */

CAR_DATA* AlleycatOwnCar(void)
{
	int id = player[0].playerCarId;

	if (id < 0 || id >= MAX_CARS)
		return NULL;

	return &car_data[id];
}

ALLEYCAT_CARSTATE AlleycatReadCar(CAR_DATA* car)
{
	ALLEYCAT_CARSTATE s;
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
	if (gAlleycat.prevDir >= 0)
	{
		s.yaw = ((s.dir - gAlleycat.prevDir + 2048) & 0xfff) - 2048;

		if (s.yaw > 256) s.yaw = 256;
		if (s.yaw < -256) s.yaw = -256;
	}

	gAlleycat.prevDir = s.dir;

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

static int AlleycatAbs(int v)
{
	return v < 0 ? -v : v;
}

int AlleycatDriveBuildMap(CAR_DATA* car)
{
	if (car == NULL)
		return 0;

	/* One build per frame: a behavior that re-picks its goal builds the window
	 * to choose on, and the forced re-plan right after it used to build the
	 * same window again - 2 x 2,401 engine probes in one frame. */
	if (gAlleycat.mapBuiltAt == gAlleycat.tick && AlleycatMapValid(&gAlleycat.map))
		return 1;

	if (!AlleycatMapBuild(&gAlleycat.map, car->hd.where.t[0], car->hd.where.t[1], car->hd.where.t[2]))
		return 0;

	gAlleycat.mapBuiltAt = gAlleycat.tick;

	return 1;
}

int AlleycatDrivePlanTo(CAR_DATA* car, int toX, int toZ, int force)
{
	int cx, cz, age, stale;

	if (car == NULL)
		return 0;

	cx = car->hd.where.t[0];
	cz = car->hd.where.t[2];

	age = gAlleycat.tick - gAlleycat.plannedAt;

	stale = force || !gAlleycat.havePlan ||
		age >= (gAlleycat.replanMs * 30) / 1000 ||
		AlleycatAbs(toX - gAlleycat.goalX) >= gAlleycat.replanMove ||
		AlleycatAbs(toZ - gAlleycat.goalZ) >= gAlleycat.replanMove;

	if (!stale)
		return 1;

	/* Rebuild the window around the car and re-plan. A build that fails means
	 * the world is not there (frontend, between levels): keep any plan we had. */
	if (!AlleycatDriveBuildMap(car))
		return gAlleycat.havePlan;

	gAlleycat.map.turnCost = gAlleycat.turnCost;

	/* While being CHASED, the pull back to the road is what makes the car lose
	 * its line and its speed, so it is weakened (see ALLEYCAT_CFG_PANIC_OFFROAD). */
	gAlleycat.map.offroadCost = gAlleycat.panicFlee ? gAlleycat.panicOffroad : gAlleycat.offroadCost;
	gAlleycat.incomplete = !AlleycatStarPlan(&gAlleycat.map, cx, cz, toX, toZ, &gAlleycat.path);
	gAlleycat.plannedAt = gAlleycat.tick;
	gAlleycat.goalX = toX;
	gAlleycat.goalZ = toZ;
	gAlleycat.havePlan = (gAlleycat.path.waypoints > 0);

	return gAlleycat.havePlan;
}

static long long AlleycatSegLen(long long ax, long long az, long long bx, long long bz)
{
	double dx = (double)(bx - ax);
	double dz = (double)(bz - az);

	return (long long)sqrt(dx * dx + dz * dz);
}

/* Where the car sits ALONG the plan: the arc length to the closest point of the
 * polyline. 0 when there is no plan. The math is the pure, 64-bit
 * AlleycatPathProjectArc (alleycat_mapgrid.c): with a 32-bit `long` (Windows) the old
 * inline copy wrapped on any segment past ~1,300 units. */
static long long AlleycatProjectArc(int cx, int cz, long long* crossD2)
{
	return AlleycatPathProjectArc(gAlleycat.path.wx, gAlleycat.path.wz, gAlleycat.path.waypoints, cx, cz, crossD2);
}

ALLEYCAT_CURVE AlleycatScanCurve(int cx, int cz, int horizon)
{
	ALLEYCAT_CURVE c;
	int n = gAlleycat.path.waypoints, k;
	long long startArc, arc = 0;
	int sharp = 0, sharpDist = 0, sharpSign = 0, sharpSpan = 0, haveCurrent = 0;

	c.sharp = 0;
	c.dist = 0;
	c.sign = 0;
	c.current = 0;
	c.span = 0;

	if (!gAlleycat.havePlan || n < 3)
		return c;

	startArc = AlleycatProjectArc(cx, cz, NULL);

	/* At each interior waypoint, the turn is the heading change from the leg in
	 * to the leg out. Track the biggest within the horizon (so the speed model
	 * slows for the real corner, not a kink), and the very next one ahead. */
	for (k = 1; k < n - 1; k++)
	{
		int ax = gAlleycat.path.wx[k - 1], az = gAlleycat.path.wz[k - 1];
		int bx = gAlleycat.path.wx[k], bz = gAlleycat.path.wz[k];
		int ex = gAlleycat.path.wx[k + 1], ez = gAlleycat.path.wz[k + 1];
		long long legIn = AlleycatSegLen(ax, az, bx, bz);
		long long legOut = AlleycatSegLen(bx, bz, ex, ez);
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
#define ALLEYCAT_SLIP_LIMIT	300

/* How far over the corner speed the car must be before the driver brakes rather
 * than merely lifting off (engine speed units). A small margin keeps it from
 * dithering between throttle and brake. */
#define ALLEYCAT_BRAKE_BAND	25

/* The speed that keeps the car inside the lateral-g budget through a corner of
 * radius r. corner_g folds the grip and the unit conversions into one knob:
 * bigger = the car carries more speed through the same curve. */
static int AlleycatSpeedForRadius(int r)
{
	int v;

	if (r <= 0)
		return gAlleycat.topSpeed;

	v = (int)sqrt((double)gAlleycat.cornerG * (double)r);

	if (v > gAlleycat.topSpeed) v = gAlleycat.topSpeed;
	if (v < 8) v = 8;

	return v;
}

/* The speed the driver may carry RIGHT NOW so it can still be down to the corner
 * speed by the time it reaches the corner: v^2 = vCorner^2 + 2*aBrake*dist.
 * This is the momentum term - slowing only once the corner arrives is exactly
 * how a car ploughs into the outside wall. */
static int AlleycatSpeedTargetRaw(void);

/* The corner/brake model's speed, under the behavior's own cruise cap when it
 * has one (Evade's pace over the road limit; 0 = no cap). */
int AlleycatSpeedTarget(void)
{
	int v = AlleycatSpeedTargetRaw();

	if (gAlleycat.speedCap > 0 && v > gAlleycat.speedCap)
		v = gAlleycat.speedCap;

	return v;
}

static int AlleycatSpeedTargetRaw(void)
{
	const ALLEYCAT_CURVE* c = &gAlleycat.curve;
	int r, vCorner, vNow;
	double turnRad;

	/* PANIC: running from the police, the point is to be gone, not to be tidy.
	 * Ignore the corner model and the aggression dial entirely and ask for the
	 * car's whole top speed - the steering, the feelers and the stuck recovery
	 * still keep it off the walls. */
	if (gAlleycat.panicFlee)
		return gAlleycat.topSpeed;

	if (!gAlleycat.havePlan || c->sharp == 0 || c->span <= 0)
		return gAlleycat.topSpeed;

	turnRad = (double)(c->sharp < 0 ? -c->sharp : c->sharp) * 6.283185307 / 4096.0;

	if (turnRad < 0.02)
		return gAlleycat.topSpeed;

	r = (int)((double)c->span / turnRad);
	vCorner = AlleycatSpeedForRadius(r);

	vNow = (int)sqrt((double)vCorner * (double)vCorner + 2.0 * (double)gAlleycat.brakeG * (double)c->dist);

	/* the aggression dial: scale the whole thing, then cap at the car's limit */
	if (gAlleycat.speedScale > 0 && gAlleycat.speedScale != 100)
		vNow = vNow * gAlleycat.speedScale / 100;

	if (vNow > gAlleycat.topSpeed) vNow = gAlleycat.topSpeed;

	return vNow;
}

/* How much grip is left: 100 when hooked up, falling as the car slides. The slip
 * angle is the honest "am I asking for more than the tyres have" measure. */
static int AlleycatGripPct(void)
{
	int s = gAlleycat.carState.slip;

	if (s < 0)
		s = -s;

	if (s >= ALLEYCAT_SLIP_LIMIT)
		return 0;

	return 100 - s * 100 / ALLEYCAT_SLIP_LIMIT;
}

/* cos(angle) fixed-point: a point is "not behind" the car when the cosine
 * between the car's heading and the direction to the point exceeds this
 * (-0.35 ~= 110 deg, so a perpendicular intersection turn still counts). */
#define ALLEYCAT_AIM_BEHIND_COS	(-1450)

/* How much further up the plan the aim looks per unit of speed. A fixed
 * look-ahead makes a fast car chase every wiggle of the path and saw at the
 * wheel; stretching it with speed is the cheapest cure for that oscillation. */
#define ALLEYCAT_AIM_SPEED_GAIN	8

/* The steering lead is configurable (`yaw_lead`, default 4): the number of frames
 * of the car's own yaw the aim leads the heading error by. It damps the wheel so
 * the car does not saw at it or oscillate up a straight, but too much of it
 * straightens the wheel as soon as the car begins turning - which reads as "it
 * steers far too slowly". See ALLEYCAT_CFG_YAW_LEAD. */

int AlleycatDriveAim(int* aimX, int* aimZ)
{
	CAR_DATA* car = AlleycatOwnCar();
	int n, cx, cz, dir, fwdX, fwdZ;
	int bestX = 0, bestZ = 0, segDx = 0, segDz = 0;
	long long bestD2 = -1, bestArc;
	long long la;

	if (car == NULL || !gAlleycat.havePlan || gAlleycat.path.waypoints <= 0)
		return 0;

	/* a longer look-ahead at speed: smoother, no sawing */
	la = (long long)gAlleycat.lookAhead + (long long)AlleycatAbs(car->hd.speed) * ALLEYCAT_AIM_SPEED_GAIN;

	/* ...and a MUCH longer one while being chased: aiming further down the road
	 * is what lets the car commit to a line and keep its speed up, instead of
	 * correcting short and often (see ALLEYCAT_CFG_PANIC_LOOKAHEAD). */
	if (gAlleycat.panicFlee)
		la += gAlleycat.panicLookAhead;

	gAlleycat.aimLook = (int)la;   /* the once-a-second log reads this */

	n = gAlleycat.path.waypoints;
	cx = car->hd.where.t[0];
	cz = car->hd.where.t[2];
	dir = car->hd.direction;
	fwdX = RSIN(dir);
	fwdZ = RCOS(dir);

	/* debug override: a fixed aim heading, to test the steering in isolation */
	if (gAlleycat.aimTest >= 0)
	{
		int d = gAlleycat.aimTest & 0xfff;

		gAlleycat.aimX = cx + (int)(4000LL * RSIN(d) / 4096);
		gAlleycat.aimZ = cz + (int)(4000LL * RCOS(d) / 4096);

		if (aimX) *aimX = gAlleycat.aimX;
		if (aimZ) *aimZ = gAlleycat.aimZ;

		return 1;
	}

	/* where the car is ALONG the plan: the closest point on the polyline */
	bestArc = AlleycatProjectArc(cx, cz, &bestD2);

	/* cross-track: how far the car sits off the planned line - the term that
	 * turns a small lateral error into a large bearing error when the look-ahead
	 * is short. Magnitude only; the steer trace reads it. */
	gAlleycat.aimOff = (bestD2 > 0) ? (int)sqrt((double)bestD2 + 0.5) : 0;

	/* Walk FORWARD from there and take the first point that is both at least
	 * look_ahead further along AND not behind the car (the DIRECTIONAL GUIDE).
	 * Skimming the segment in ~512-unit steps means a point behind the car is
	 * stepped over rather than aimed at, so the car never turns back for a
	 * waypoint it has already passed. Falls through to the goal if the whole
	 * remaining plan is behind - then a turn really is the only way on. The
	 * walk is the pure AlleycatPathAimPoint (alleycat_mapgrid.c, host-tested). */
	AlleycatPathAimPoint(gAlleycat.path.wx, gAlleycat.path.wz, n, cx, cz, fwdX, fwdZ,
		bestArc, la, ALLEYCAT_AIM_BEHIND_COS, &bestX, &bestZ, &segDx, &segDz);

	if (aimX) *aimX = bestX;
	if (aimZ) *aimZ = bestZ;
	gAlleycat.aimX = bestX;
	gAlleycat.aimZ = bestZ;

	/* how far ahead the aim actually landed - the once-a-second log prints it,
	 * and it is the honest readout for "is it aiming down the road or at its own
	 * bumper" (a look-ahead the plan cannot satisfy falls back to the goal). */
	gAlleycat.aimDist = (int)sqrt((double)AlleycatDist2(bestX, bestZ, cx, cz) + 0.5);

	/* the PATH's own direction where the aim sits - the bearing of the plan
	 * segment that carries it, NOT the bearing from the car to it (that is
	 * `want`, and using it here would make the readout tautological). With
	 * aimOff this splits the heading error into "the car is not aligned with
	 * the path" and "the aim sits off to one side of the car". */
	gAlleycat.aimPathDir = (segDx != 0 || segDz != 0) ? (ratan2(segDx, segDz) & 0xfff) : 0;

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
#define ALLEYCAT_AHEAD_NONE	0x7fffffff

static int AlleycatCarAhead(CAR_DATA* car, long long* dist2Out, int* relOut)
{
	int i;
	int cx = car->hd.where.t[0];
	int cz = car->hd.where.t[2];
	int range = gAlleycat.avoidDist;
	long long range2;
	long long bestD2 = -1;
	int bestRel = 0;

	if (dist2Out) *dist2Out = -1;
	if (relOut) *relOut = 0;

	if (range > ALLEYCAT_AVOID_MAX)
	{
		/* The cap is deliberate - a car does not brake for traffic half a street
		 * away - but it used to be SILENT, so `avoid_dist = 1800` in the config
		 * did nothing at all and looked broken. Say so once. */
		static int warned;

		if (!warned && gAlleycat.ctx != NULL)
		{
			gAlleycat.ctx->jer_log(gAlleycat.ctx,
				"[Alleycat] avoid_dist %d exceeds the %d cap; using %d\n",
				range, ALLEYCAT_AVOID_MAX, ALLEYCAT_AVOID_MAX);
			warned = 1;
		}

		range = ALLEYCAT_AVOID_MAX;
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
		if (i == gAlleycat.followCarId)
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
		return ALLEYCAT_AHEAD_NONE;

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

static int AlleycatFeelClear(CAR_DATA* car, int off, int dist, int radius)
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
	if (AlleycatMapCellHasSmashable(&v, radius))
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
#define ALLEYCAT_FEEL_HOLD	6	/* frames a cleared feeler keeps counting */

/* The SAFE ZONE the feelers ask the scenery probe for, in world units. The old
 * 200/240 left each probe about a car's half-width wide, and a thin pole between
 * two rays was simply stepped over - the car then clipped it. A chunkier radius
 * catches poles and posts while they are still a steering correction away, not a
 * collision. (Nose is wider still: it is what triggers the brake.) */
#define ALLEYCAT_FEEL_RADIUS_SIDE	300	/* the four side feelers */
#define ALLEYCAT_FEEL_RADIUS_NOSE	360	/* the nose probe */

static int sFeelHold[5];	/* [0..3] the four side feelers, [4] the nose */

static int AlleycatFeelBlocked(int which, int clear)
{
	if (!clear)
	{
		sFeelHold[which] = ALLEYCAT_FEEL_HOLD;
		return 1;
	}

	if (sFeelHold[which] > 0)
	{
		sFeelHold[which]--;
		return 1;
	}

	return 0;
}

static int AlleycatFeelers(CAR_DATA* car, int* frontBlocked)
{
	int bias = 0;

	*frontBlocked = 0;

	/* left feelers push right (negative), right feelers push left (positive) */
	if (AlleycatFeelBlocked(0, AlleycatFeelClear(car, 512, 900, ALLEYCAT_FEEL_RADIUS_SIDE)))	bias -= 300;
	if (AlleycatFeelBlocked(1, AlleycatFeelClear(car, 256, 1200, ALLEYCAT_FEEL_RADIUS_SIDE)))	bias -= 150;
	if (AlleycatFeelBlocked(2, AlleycatFeelClear(car, -256, 1200, ALLEYCAT_FEEL_RADIUS_SIDE)))	bias += 150;
	if (AlleycatFeelBlocked(3, AlleycatFeelClear(car, -512, 900, ALLEYCAT_FEEL_RADIUS_SIDE)))	bias += 300;

	if (AlleycatFeelBlocked(4, AlleycatFeelClear(car, 0, 800, ALLEYCAT_FEEL_RADIUS_NOSE)))	*frontBlocked = 1;

	return bias;
}

/* SLEW LIMIT on the aim nudge. The hold above ramps a single feeler, but several
 * can still flip in one frame (a gap between two probes opens and closes) and the
 * traffic bias then adds another 220 on top, so the TOTAL is moved toward its new
 * value by at most ALLEYCAT_BIAS_SLEW a frame rather than assigned. A step into the
 * aim is a step into the heading loop, and that is what the wheel saws on. */
#define ALLEYCAT_BIAS_SLEW	120	/* aim units per frame */

static int sBiasApplied;

static int AlleycatBiasSlew(int want)
{
	int d = want - sBiasApplied;

	if (d > ALLEYCAT_BIAS_SLEW)
		d = ALLEYCAT_BIAS_SLEW;
	else if (d < -ALLEYCAT_BIAS_SLEW)
		d = -ALLEYCAT_BIAS_SLEW;

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
#define ALLEYCAT_STUCK_FRAMES	40	/* ~1.3 s at the fixed 30 Hz logic rate */
#define ALLEYCAT_RECOVER_FRAMES	25

/* TIGHT TURN (CAR_PAD_FASTSTEER = L1): handling.c doubles the lock rate (64 vs
 * 32 a frame) and raises the stop from 352 to 511 while that bit is held. The
 * driver asks for it when the damped heading error is a real turn's worth, so
 * straight-line corrections keep the slower, smoother rate. 128 game units is
 * about 11 degrees. Never on the emergency paths below (handbrake turn, reverse
 * burst): those return an explicit pad before this is reached. */
#define ALLEYCAT_FASTSTEER_BAND	128

static int sStuckFrames;
static int sRecoverFrames;
static int sHasMoved;	/* only start counting "stuck" once the car has driven */

static unsigned int AlleycatSteerPadImpl(CAR_DATA* car, int aimX, int aimZ)
{
	int dx = aimX - car->hd.where.t[0];
	int dz = aimZ - car->hd.where.t[2];
	int want = ratan2(dx, dz) & 0xfff;
	int frontBlocked = 0;
	int bias = AlleycatFeelers(car, &frontBlocked);
	int diff, spd;
	int ahead;
	long long aheadD2 = -1;
	int aheadRel = 0;
	unsigned int pad = 0;

	/* what the car is doing right now - speed, yaw rate, slide. The controller
	 * reads this; it is the difference between driving and walking a line. */
	gAlleycat.carState = AlleycatReadCar(car);
	gAlleycat.wantHandbrake = 0;

	/* and what the plan is about to do - the corner ahead and how far off */
	gAlleycat.curve = AlleycatScanCurve(car->hd.where.t[0], car->hd.where.t[2], gAlleycat.cornerLookahead);

	/* the grip-limited speed the car may carry into that corner, and how much
	 * grip is left right now */
	gAlleycat.targetSpeed = AlleycatSpeedTarget();
	gAlleycat.gripPct = AlleycatGripPct();

	/* Another car close ahead in our lane: steer around it AND be ready to brake.
	 * The steer bias picks the side with room (or the side AWAY from the car), so
	 * it goes around rather than into it; the brake in the pedal section below is
	 * the backstop for when there is no room to go around at all. */
	ahead = AlleycatCarAhead(car, &aheadD2, &aheadRel);

	if (ahead != ALLEYCAT_AHEAD_NONE)
	{
		int rightClear = AlleycatFeelClear(car, 512, 1400, ALLEYCAT_FEEL_RADIUS_SIDE);
		int leftClear = AlleycatFeelClear(car, -512, 1400, ALLEYCAT_FEEL_RADIUS_SIDE);

		if (rightClear && !leftClear)
			bias += 220;		/* room to the right -> aim right */
		else if (leftClear && !rightClear)
			bias -= 220;		/* room to the left -> aim left */
		else
			bias += (aheadRel > 0) ? -220 : 220;	/* else away from the car */

	}

	/* nudge the aim around what the plan could not see (the feelers + traffic).
	 * SLEWED, not assigned - see AlleycatBiasSlew. */
	bias = AlleycatBiasSlew(bias);
	gAlleycat.lastBias = bias;

	want = (want + bias) & 0xfff;

	diff = ((want - car->hd.direction + 2048) & 0xfff) - 2048;
	spd = car->hd.speed;

	gAlleycat.lastDiff = diff;

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

	gAlleycat.stuckFrames = sStuckFrames;
	gAlleycat.recoverFrames = sRecoverFrames;

	if (sRecoverFrames > 0)
	{
		sRecoverFrames--;

		/* back up, steering so the NOSE swings toward the aim (the bit is
		 * inverted because we are reversing - see ALLEYCAT_RECOVER_FRAMES) */
		gAlleycat.recoverFrames = sRecoverFrames;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	if (sStuckFrames >= ALLEYCAT_STUCK_FRAMES)
	{
		sStuckFrames = 0;
		sRecoverFrames = ALLEYCAT_RECOVER_FRAMES;

		gAlleycat.stuckFrames = 0;
		gAlleycat.recoverFrames = sRecoverFrames;

		/* the plan is what led us here: throw it away so the next frame re-plans
		 * from wherever the reverse burst leaves us, and ask the behavior for a
		 * destination it can actually reach - WILD, i.e. anywhere in the window,
		 * so the car relocates instead of niggling at the same wedge */
		gAlleycat.havePlan = 0;
		gAlleycat.forceNewGoal = 1;
		gAlleycat.reposition = 1;

		return CAR_PAD_BRAKE | ((diff > 0) ? CAR_PAD_LEFT : CAR_PAD_RIGHT);
	}

	/* --- wall evasion: a tight handbrake turn when the nose gets close -----
	 * The handbrake is NOT sent as a pad bit: handling.c cycles
	 * g_PlayerControlMode on that bit's press edge (manual -> traffic -> cop ->
	 * lead), which would hand the car to the engine's AI. The CAR_PAD hook's
	 * handled path sets cp->handbrake directly, so the mode is never touched. */
	if (spd > 12 && !AlleycatFeelClear(car, 0, 1100, ALLEYCAT_FEEL_RADIUS_NOSE))
	{
		int rightClear = AlleycatFeelClear(car, 512, 1100, ALLEYCAT_FEEL_RADIUS_SIDE);
		int leftClear = AlleycatFeelClear(car, -512, 1100, ALLEYCAT_FEEL_RADIUS_SIDE);

		gAlleycat.wantHandbrake = 1;
		pad = 0;	/* a handbrake turn drives no throttle */

		/* turn toward whichever side is open; if neither clearly is, toward the
		 * aim */
		if (rightClear && !leftClear)
			pad |= CAR_PAD_RIGHT;
		else if (leftClear && !rightClear)
			pad |= CAR_PAD_LEFT;
		else
			pad |= (diff > 0) ? CAR_PAD_RIGHT : CAR_PAD_LEFT;

		gAlleycat.lastSteerDir = 0;

		return pad;
	}

	/* Scenery right on the nose AND barely moving: back up NOW rather than wait
	 * out the whole stuck timer - the earlier the car peels off, the less it
	 * grinds along the wall. */
	if (spd < 15 && !AlleycatFeelClear(car, 0, 480, ALLEYCAT_FEEL_RADIUS_NOSE))
	{
		sRecoverFrames = ALLEYCAT_RECOVER_FRAMES;
		gAlleycat.lastSteerDir = 0;

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
	else if (spd > gAlleycat.targetSpeed + ALLEYCAT_BRAKE_BAND)
	{
		pad = CAR_PAD_BRAKE;		/* well over the corner speed: brake */
	}
	else if (spd > gAlleycat.targetSpeed)
	{
		pad = 0;			/* a little over: just lift off */
	}
	else if (spd < gAlleycat.targetSpeed - 12)
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
		int pred = diff - gAlleycat.carState.yaw * gAlleycat.yawLead;
		int apred = pred < 0 ? -pred : pred;
		int dir = pred > 0 ? 1 : -1;

		gAlleycat.lastPred = pred;	/* the steer trace reads this */

		if (gAlleycat.lastSteerDir == dir && apred > holdBand)
			;					/* stay on it */
		else if (apred > startBand)
			gAlleycat.lastSteerDir = dir;
		else
			gAlleycat.lastSteerDir = 0;

		/* TIGHT TURN: a real heading error gets the fast lock rate (see
		 * ALLEYCAT_FASTSTEER_BAND). Added here, where `apred` is the damped
		 * error the decision actually used. */
		if (gAlleycat.lastSteerDir != 0 && apred > ALLEYCAT_FASTSTEER_BAND)
			pad |= (unsigned int)CAR_PAD_FASTSTEER;
	}

	if (gAlleycat.lastSteerDir > 0)
		pad |= CAR_PAD_RIGHT;
	else if (gAlleycat.lastSteerDir < 0)
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
	if (ahead != ALLEYCAT_AHEAD_NONE)
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
unsigned int AlleycatDriveSteerPad(CAR_DATA* car, int aimX, int aimZ)
{
	unsigned int pad = AlleycatSteerPadImpl(car, aimX, aimZ);

	gAlleycat.lastPad = (int)pad;

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
	if (gAlleycat.panicFlee && gAlleycat.panicWheelspin && car != NULL &&
	    (pad & CAR_PAD_ACCEL) && AlleycatAbs(gAlleycat.lastDiff) < 150 &&
	    AlleycatAbs(car->wheel_angle) < 80)
	{
		pad = (pad & ~(unsigned int)CAR_PAD_ACCEL) | (unsigned int)CAR_PAD_WHEELSPIN;
		gAlleycat.lastPad = (int)pad;
	}

	/* STEERING CAP (Evade's `max_steer`): stop adding lock once the front wheel
	 * is at the cap. Alleycat steers with the regular D-pad rate, which the engine
	 * tops out at 352 (handling.c), so a cap of 352 or more changes nothing. Not
	 * applied to the emergency paths (the wall-evasion handbrake turn, the
	 * reverse burst), which need the whole lock. */
	if (car != NULL && gAlleycat.steerCap > 0 && gAlleycat.steerCap < 352 &&
	    !gAlleycat.wantHandbrake && gAlleycat.recoverFrames == 0)
	{
		if ((pad & CAR_PAD_RIGHT) && car->wheel_angle >= gAlleycat.steerCap)
			pad &= ~(unsigned int)CAR_PAD_RIGHT;

		if ((pad & CAR_PAD_LEFT) && car->wheel_angle <= -gAlleycat.steerCap)
			pad &= ~(unsigned int)CAR_PAD_LEFT;

		gAlleycat.lastPad = (int)pad;
	}

	/* the ACTUAL front wheel angle, read back after the engine applied the pad
	 * (30 Hz logic, one frame behind). This is the readout that shows the wheel
	 * being centred between corrections - see ALLEYCAT_CFG_STEER_TRACE. */
	if (car != NULL)
		gAlleycat.lastWheel = car->wheel_angle;

	return pad;
}

/* Hold: come to a stop and stay there. See alleycat_drive.h for why this is a
 * separate primitive rather than a flag on the steering pad. */
unsigned int AlleycatDriveHoldPad(CAR_DATA* car)
{
	int spd;
	unsigned int pad;

	if (car == NULL)
		return 0;

	spd = car->hd.speed;

	if (spd < 0)
		spd = -spd;

	/* The once-a-second log prints carState and the last-* fields, and this path
	 * never reaches AlleycatSteerPadImpl to fill them - so refresh them here or the
	 * readout freezes at whatever the last real steering frame left. */
	gAlleycat.carState = AlleycatReadCar(car);
	gAlleycat.targetSpeed = 0;
	gAlleycat.lastDiff = 0;
	gAlleycat.lastBias = 0;
	gAlleycat.lastSteerDir = 0;
	gAlleycat.wantHandbrake = 0;

	/* parked on purpose is not wedged: the stop itself must not count toward the
	 * stuck timer, or pulling away again (a light going green, the followed car
	 * leaving) starts most of the way to a reverse burst */
	sStuckFrames = 0;
	gAlleycat.stuckFrames = 0;

	pad = (spd > 8) ? CAR_PAD_BRAKE : 0;

	gAlleycat.lastPad = (int)pad;

	return pad;
}

/* A new level (JER_EVENT_GAME_START): everything here describes the LAST one. */
void AlleycatDriveResetRun(void)
{
	int i;

	for (i = 0; i < 5; i++)
		sFeelHold[i] = 0;

	sBiasApplied = 0;
	sStuckFrames = 0;
	sRecoverFrames = 0;
	sHasMoved = 0;

	gAlleycat.havePlan = 0;
	gAlleycat.incomplete = 0;
	gAlleycat.path.waypoints = 0;
	gAlleycat.path.complete = 0;
	gAlleycat.plannedAt = 0;
	gAlleycat.goalX = gAlleycat.goalZ = 0;
	gAlleycat.aimX = gAlleycat.aimZ = 0;
	gAlleycat.map.valid = 0;
	gAlleycat.mapBuiltAt = -1;
	gAlleycat.prevDir = -1;		/* no last heading: the first frame has no yaw */
	gAlleycat.lastSteerDir = 0;
	gAlleycat.lastDiff = gAlleycat.lastBias = gAlleycat.lastPred = 0;
	gAlleycat.wantHandbrake = 0;
	gAlleycat.stuckFrames = gAlleycat.recoverFrames = 0;
	gAlleycat.forceNewGoal = 0;
	gAlleycat.reposition = 0;
	gAlleycat.tanActive = 0;
	gAlleycat.followCarId = -1;
	gAlleycat.panicFlee = 0;
	gAlleycat.speedCap = 0;
	gAlleycat.steerCap = 0;
}

/* ------------------------------------------------------------------ */
/* the road: its speed limit and its traffic lights (Evade)             */
/* ------------------------------------------------------------------ */

/* The road surface under a world point, probed at the ground there (the same
 * way the occupancy grid reads JerRoadAt). 0 off the level or off the road. */
static int AlleycatRoadUnder(int x, int z, JER_ROAD_INFO* out)
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

int AlleycatDriveRoadLimit(CAR_DATA* car, int fallback)
{
	JER_ROAD_INFO info;
	int id;

	if (car == NULL || !AlleycatRoadUnder(car->hd.where.t[0], car->hd.where.t[2], &info))
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
int AlleycatDriveStopLightAhead(CAR_DATA* car, int range, int* distOut)
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
	if (!AlleycatRoadUnder(cx, cz, &here) || here.kind == 2)
		return 0;

	for (d = 300; d <= range; d += 300)
	{
		int px = cx + (int)((long long)d * RSIN(dir) / 4096);
		int pz = cz + (int)((long long)d * RCOS(dir) / 4096);
		int exitId = -1, phase;

		if (!AlleycatRoadUnder(px, pz, &ahead))
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
