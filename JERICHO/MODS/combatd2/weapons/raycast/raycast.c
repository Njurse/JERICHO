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

void cd2RaycastSpawn(const CD2_WEAPON_DEF* def, const CAR_DATA* shooter,
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
		return;
	}
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
			int j;

			r->pos.vx += r->vel.vx / steps;
			r->pos.vy += r->vel.vy / steps;
			r->pos.vz += r->vel.vz / steps;

			// car hit (skip the shooter)
			for (j = 0; j < MAX_CARS; j++)
			{
				CAR_DATA* cp = &car_data[j];

				if (cp == r->owner || cp->controlType == 0 ||
				    cp->ap.carCos == NULL)
					continue;

				if (cd2WpnPointInCar(cp, &r->pos))
				{
					cd2WpnDamageCar(cp, &r->pos, r->def->damage);
					cd2WpnMark(&r->pos, 255, 255, 180);
					r->active = 0;
					break;
				}
			}

			if (!r->active)
				break;

			// ground hit
			{
				int gh = MapHeight(&r->pos);

				if (gh != 0 && r->pos.vy <= gh + 8)
				{
					cd2WpnMark(&r->pos, 200, 200, 200);
					r->active = 0;
					break;
				}
			}
		}

		// scenery collision (buildings/walls): the depth-segment test the
		// engine's pathfinder/look code uses (0 = blocked)
		if (r->active && r->def->collideScenery &&
		    lineClear(&r->prev, &r->pos) == 0)
		{
			cd2WpnMark(&r->pos, 210, 210, 210);
			r->active = 0;
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
