// weapons/raycast/raycast.c — RAYCAST class pool.
//
// A RAYCAST weapon is a FAST MOVING PARTICLE, not an instant hitscan: each
// shot is one point that advances def->speed world units per frame and tests
// a car/ground hit as it flies (with sub-steps so a fast bullet can't tunnel
// through a car). On a car hit it applies damage and drops a visible impact
// marker; on ground it just stops. The drawn "tracer" is the particle's own
// prev->pos streak, so it visibly travels instead of snapping muzzle->target.

#include "driver2.h"
#include "cars.h"
#include "cosmetic.h"
#include "dr2roads.h"
#include "objcoll.h"
#include "system.h"
#include "jer_math.h"
#include "combatd2.h"
#include "weapons/core/weapon.h"
#include "weapons/core/weapon_internal.h"

#include <string.h>

#define CD2_MAX_RAYCAST		64
#define CD2_RAY_SUBSTEP		32	// world units per sub-step (tunneling guard)
#define CD2_RAY_MAX_SUBSTEP	16

typedef struct CD2_RAYCAST
{
	int active;
	const CD2_WEAPON_DEF* def;
	const CAR_DATA* owner;
	VECTOR pos;	// y-up world
	VECTOR prev;	// last frame's pos (streak tail)
	VECTOR vel;	// world units / frame
	int travelled;	// world units flown (vs def->range)
} CD2_RAYCAST;

static CD2_RAYCAST gRcast[CD2_MAX_RAYCAST];

void cd2RaycastReset(void)
{
	int i;

	for (i = 0; i < CD2_MAX_RAYCAST; i++)
		gRcast[i].active = 0;
}

int cd2RaycastSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
		     const VECTOR* from, const VECTOR* dir)
{
	int i;
	int speed = (def != NULL) ? def->speed : 0;
	VECTOR carVel;

	// inertial launch: the shot inherits the shooter's velocity so it always
	// moves away from the car instead of trailing it at high speed
	if (shooter != NULL)
		cd2WpnCarVelocity(shooter, &carVel);
	else
		carVel.vx = carVel.vy = carVel.vz = 0;

	for (i = 0; i < CD2_MAX_RAYCAST; i++)
	{
		CD2_RAYCAST* r = &gRcast[i];

		if (r->active)
			continue;

		r->active = 1;
		r->def = def;
		r->owner = shooter;
		r->pos = *from;
		r->prev = *from;
		r->vel.vx = (int)(((long long)dir->vx * speed) >> 12) + carVel.vx;
		r->vel.vy = (int)(((long long)dir->vy * speed) >> 12) + carVel.vy;
		r->vel.vz = (int)(((long long)dir->vz * speed) >> 12) + carVel.vz;
		r->travelled = 0;
		return 1;
	}

	return 0;	// pool full
}

// Shotgun scatter: fire `count` pellets at once, ALTERNATING the LEFT/RIGHT
// fender muzzles, each direction jittered within a car-relative cone and
// biased outward per fender so the two barrels visibly fan apart. The angles
// are small, so dir = forward + right*jh + up*jv needs no renormalising (the
// magnitude stays ~4096).
void cd2RaycastScatter(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
		       int count, int spread, int fanout)
{
	const MATRIX* w;
	VECTOR fwd;
	int i, spawned = 0;

	if (def == NULL || shooter == NULL || shooter->ap.carCos == NULL || count <= 0)
		return;

	w = &shooter->hd.where;

	cd2WpnForward(shooter, &fwd);

	for (i = 0; i < count; i++)
	{
		VECTOR muzzle, dir;
		int side = (i & 1) ? 1 : -1;			// alternate fenders
		int jh = cd2WpnRand(spread * 2 + 1) - spread + side * fanout;
		int jv = cd2WpnRand(spread + 1) - spread / 2;	// less vertical
		long long rx = w->m[0][0], ry = w->m[1][0], rz = w->m[2][0];	// right
		long long ux = w->m[0][1], uy = w->m[1][1], uz = w->m[2][1];	// up

		// a leaning weapon (the shotgun) fires from the driver's window; a
		// non-leaning one from the alternating fenders
		cd2WpnShotMuzzle(def, shooter, side, &muzzle);

		dir.vx = fwd.vx + (int)((rx * jh + ux * jv) >> 12);
		dir.vy = fwd.vy + (int)((ry * jh + uy * jv) >> 12);
		dir.vz = fwd.vz + (int)((rz * jh + uz * jv) >> 12);

		spawned += cd2RaycastSpawn(def, shooter, &muzzle, &dir);
	}

	if (gCd2Cfg.debugLog)
		printInfo("[combatd2] shotgun blast: %d/%d pellets from both fenders\n", spawned, count);
}

void cd2RaycastStep(void)
{
	int i;

	for (i = 0; i < CD2_MAX_RAYCAST; i++)
	{
		CD2_RAYCAST* r = &gRcast[i];
		int steps, s;

		if (!r->active)
			continue;

		r->prev = r->pos;

		// sub-step on the ACTUAL per-frame distance (which grows once the
		// shooter's velocity is added) so a fast shot can't tunnel a car
		{
			int ax = ABS(r->vel.vx), ay = ABS(r->vel.vy), az = ABS(r->vel.vz);
			int mag = ((ax < az) ? az : ax) + ((ax < az) ? ax : az) / 2 + ay / 2;

			steps = mag / CD2_RAY_SUBSTEP;
			if (steps < 1)
				steps = 1;
			if (steps > CD2_RAY_MAX_SUBSTEP)
				steps = CD2_RAY_MAX_SUBSTEP;

			r->travelled += mag;
		}

		for (s = 0; s < steps && r->active; s++)
		{
			int j, gh;
			VECTOR stepPrev = r->pos;

			r->pos.vx += r->vel.vx / steps;
			r->pos.vy += r->vel.vy / steps;
			r->pos.vz += r->vel.vz / steps;

			// vehicle hit (skip the shooter): run the impact code now
			for (j = 0; j < MAX_CARS; j++)
			{
				CAR_DATA* cp = &car_data[j];

				if (cp == r->owner || cp->controlType == 0 ||
				    cp->ap.carCos == NULL)
					continue;

				if (cd2WpnPointInCar(cp, &r->pos))
				{
					cd2WpnDamageCar(cp, &r->pos, r->def->damage, r->owner);
					cd2WpnKnock(cp, &r->pos, &r->vel, r->def->damage);
					cd2WpnMark(&r->pos, r->def->colR, r->def->colG, r->def->colB);
					r->active = 0;
					break;
				}
			}

			if (!r->active)
				break;

			// scenery hit on this sub-step (walls stop the bullet immediately,
			// before anything behind them)
			if (r->def->collideScenery && lineClear(&stepPrev, &r->pos) == 0)
			{
				cd2WpnMark(&r->pos, 210, 210, 210);
				r->active = 0;
				break;
			}

			// ground hit
			gh = MapHeight(&r->pos);
			if (gh != 0 && r->pos.vy <= gh + 8)
			{
				cd2WpnMark(&r->pos, 200, 200, 200);
				r->active = 0;
				break;
			}
		}

		if (r->active && r->travelled >= r->def->range)
			r->active = 0;
	}
}

void cd2RaycastDraw(void)
{
	int i;

	for (i = 0; i < CD2_MAX_RAYCAST; i++)
	{
		CD2_RAYCAST* r = &gRcast[i];

		if (!r->active)
			continue;

		cd2WpnLine(&r->prev, &r->pos, r->def->colR, r->def->colG, r->def->colB);
	}
}

int cd2RaycastThreat(const CAR_DATA* car, VECTOR* pos, VECTOR* vel)
{
	int i;

	for (i = 0; i < CD2_MAX_RAYCAST; i++)
	{
		CD2_RAYCAST* r = &gRcast[i];
		int dx, dy, dz, dist2, closing;

		if (!r->active || r->owner == car)
			continue;

		dx = car->hd.where.t[0] - r->pos.vx;
		dy = car->hd.where.t[1] - r->pos.vy;
		dz = car->hd.where.t[2] - r->pos.vz;
		dist2 = dx * dx + dy * dy + dz * dz;

		if (dist2 > CD2_THREAT_RANGE * CD2_THREAT_RANGE)
			continue;

		closing = r->vel.vx * dx + r->vel.vy * dy + r->vel.vz * dz;
		if (closing <= 0)
			continue;

		if (pos != NULL) *pos = r->pos;
		if (vel != NULL) *vel = r->vel;
		return 1;
	}

	return 0;
}
